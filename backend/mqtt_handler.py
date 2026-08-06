"""
mqtt_handler.py

Background MQTT subscriber that bridges the firmware's five publish
topics (fleet/{device_id}/telemetry, .../score, .../alert, .../crash,
.../status) into the database defined in database.py.

Targets paho-mqtt >= 2.0, using the CallbackAPIVersion.VERSION2 callback
signatures (paho-mqtt 2.0 changed on_connect/on_disconnect signatures
and requires explicitly selecting a callback API version).

Runs via client.loop_start(), which spawns its own background thread
inside the paho-mqtt library - start()/stop() are called once each from
app.py's FastAPI lifespan handler, not from a request handler.

FIX NOTE (see accompanying compatibility review): app.py imports this
class as `MQTTIngestHandler` and constructs it with
`(broker_host, broker_port, topic_prefix, client_id)`. The previous
version of this file defined `MQTTHandler` with a different constructor
signature (no topic_prefix) and no `is_connected()` method that
app.py's /health endpoint calls. Both are fixed below; topic_prefix
also now actually drives the wildcard subscriptions instead of the
literal string "fleet" being hardcoded.
"""

import json
import logging
from datetime import datetime, timezone
from typing import Optional

import paho.mqtt.client as mqtt

from database import (
    get_session,
    upsert_device,
    insert_telemetry,
    insert_score,
    insert_alert,
    insert_crash,
    insert_status,
)

logger = logging.getLogger("dbas.mqtt_handler")


# Message types published under fleet/{device_id}/<type> by the
# firmware (see config.h's MQTT_TOPIC_*_FMT macros), with the QoS to
# subscribe at (chosen to match, or exceed, the QoS the firmware
# publishes each message type at).
_MESSAGE_TYPE_QOS = {
    "telemetry": 1,
    "score": 1,
    "alert": 2,
    "crash": 2,
    "status": 1,
}


class MQTTIngestHandler:
    """Encapsulates the paho-mqtt client's lifecycle and message
    dispatch. One instance is created and owned by app.py."""

    def __init__(
        self,
        broker_host: str,
        broker_port: int,
        topic_prefix: str = "fleet",
        client_id: str = "dbas_backend",
        keepalive_sec: int = 60,
        username: Optional[str] = None,
        password: Optional[str] = None,
    ):
        self._broker_host = broker_host
        self._broker_port = broker_port
        self._keepalive_sec = keepalive_sec

        # topic_prefix is the fixed segment the firmware always
        # publishes under (see MQTT_TOPIC_*_FMT = "fleet/%s/<type>" in
        # config.h) - NOT the device id, which sits in the middle
        # wildcard position.
        self._topic_prefix = topic_prefix.strip("/") or "fleet"
        self._subscriptions = [
            (f"{self._topic_prefix}/+/{msg_type}", qos)
            for msg_type, qos in _MESSAGE_TYPE_QOS.items()
        ]

        self._client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=client_id,
        )
        if username:
            self._client.username_pw_set(username, password)

        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message

        # Exponential-ish backoff bounds for paho's built-in automatic
        # reconnection (triggered internally by loop_start() whenever
        # the connection drops).
        self._client.reconnect_delay_set(min_delay=1, max_delay=30)

    def start(self) -> None:
        """Connect to the broker and start the background network loop
        (paho-mqtt's own thread, separate from FastAPI's)."""
        logger.info("Connecting to MQTT broker %s:%d", self._broker_host, self._broker_port)
        self._client.connect(self._broker_host, self._broker_port, self._keepalive_sec)
        self._client.loop_start()

    def stop(self) -> None:
        """Stop the background network loop and disconnect cleanly."""
        logger.info("Stopping MQTT handler")
        self._client.loop_stop()
        self._client.disconnect()

    def is_connected(self) -> bool:
        """Whether the underlying paho client currently holds an open
        broker connection. Used by app.py's /health endpoint."""
        return self._client.is_connected()

    # -------------------------------------------------------------------
    # paho-mqtt Callbacks (CallbackAPIVersion.VERSION2 signatures)
    # -------------------------------------------------------------------

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            logger.info("Connected to MQTT broker")
            client.subscribe(self._subscriptions)
            for topic, qos in self._subscriptions:
                logger.info("Subscribed to %s (QoS %d)", topic, qos)
        else:
            logger.error("Failed to connect to MQTT broker: %s", reason_code)

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        if reason_code != 0:
            logger.warning("Unexpected MQTT disconnection (%s); paho will auto-reconnect", reason_code)
        else:
            logger.info("Disconnected from MQTT broker")

    def _on_message(self, client, userdata, msg):
        # Never let an exception here kill paho's network thread - log
        # and move on to the next message.
        try:
            self._handle_message(msg.topic, msg.payload)
        except Exception:
            logger.exception("Unhandled error processing message on topic %s", msg.topic)

    # -------------------------------------------------------------------
    # Message Handling
    # -------------------------------------------------------------------

    def _handle_message(self, topic: str, raw_payload: bytes) -> None:
        parts = topic.split("/")
        if len(parts) != 3 or parts[0] != self._topic_prefix:
            logger.warning("Ignoring message on unexpected topic shape: %s", topic)
            return

        _, device_id, message_type = parts

        try:
            payload_str = raw_payload.decode("utf-8")
            payload = json.loads(payload_str)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            logger.error("Malformed payload on topic %s: %s", topic, exc)
            return

        if not isinstance(payload, dict):
            logger.error("Payload on topic %s is not a JSON object, discarding", topic)
            return

        # Normalize timestamp_ms: ESP32 firmware often publishes uptime-based milliseconds
        # (e.g. 5000 for 5s since boot) rather than real Unix epoch milliseconds. If it's
        # too small (< 1000000000000 ms, which is Sept 2001) or missing/zero, override it
        # with current UTC epoch ms so charts, maps, and UI timestamps render correctly.
        now_ms = int(datetime.now(timezone.utc).timestamp() * 1000)
        ts = payload.get("timestamp_ms")
        if not isinstance(ts, (int, float)) or ts < 1000000000000:
            payload["timestamp_ms"] = now_ms
            if message_type == "alert":
                try:
                    alert_dict = json.loads(payload_str)
                    alert_dict["timestamp_ms"] = now_ms
                    payload_str = json.dumps(alert_dict)
                except Exception:
                    pass

        session = get_session()
        try:
            device_type = payload.get("device_type") if message_type == "status" else None
            firmware_version = payload.get("firmware_version") if message_type == "status" else None
            upsert_device(session, device_id, device_type=device_type, firmware_version=firmware_version)

            if message_type == "telemetry":
                insert_telemetry(session, device_id, payload)
            elif message_type == "score":
                insert_score(session, device_id, payload)
            elif message_type == "alert":
                insert_alert(session, device_id, payload, payload_str)
            elif message_type == "crash":
                insert_crash(session, device_id, payload)
                logger.critical("CRASH reported by device %s: %s", device_id, payload_str)
            elif message_type == "status":
                insert_status(session, device_id, payload)
            else:
                logger.warning("Unknown message type '%s' on topic %s", message_type, topic)
                session.rollback()
                return

            session.commit()
        except Exception:
            session.rollback()
            logger.exception("Failed to persist %s message from device %s", message_type, device_id)
        finally:
            session.close()

