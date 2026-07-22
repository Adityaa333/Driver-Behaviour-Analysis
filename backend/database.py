"""
database.py

SQLAlchemy models and session management for the Driver Behaviour
Analysis System backend.

Schema design mirrors the five MQTT payload types the firmware publishes
(fleet/{device_id}/telemetry, .../score, .../alert, .../crash,
.../status) plus a Device table that tracks which devices have been
seen and when. Each table is intentionally denormalized/flat (one row
per received message) rather than heavily normalized, since this is a
time-series ingestion workload: queries are almost always "give me the
last N records for device X", not complex joins.

DATABASE_URL defaults to a local SQLite file for easy local development
and grading/demo purposes; set the DATABASE_URL environment variable to
point at Postgres/MySQL/etc. in a real deployment (e.g.
"postgresql://user:pass@host/dbname"). No other code in this backend is
SQLite-specific.
"""

import os
import logging
from datetime import datetime, timezone
from typing import Optional

from sqlalchemy import (
    create_engine,
    Column,
    Integer,
    Float,
    String,
    Boolean,
    DateTime,
    BigInteger,
    Text,
    ForeignKey,
    Index,
)
from sqlalchemy.orm import declarative_base, sessionmaker, Session

logger = logging.getLogger("dbas.database")

DATABASE_URL = os.environ.get("DATABASE_URL", "sqlite:///./dbas.db")

# check_same_thread=False is required for SQLite specifically because
# mqtt_handler.py writes from the paho-mqtt background thread while
# api.py reads from FastAPI's request-handling threads/event loop. This
# flag has no effect on non-SQLite backends and is safe to leave in
# place if DATABASE_URL is changed.
_engine_kwargs = {"connect_args": {"check_same_thread": False}} if DATABASE_URL.startswith("sqlite") else {}

engine = create_engine(DATABASE_URL, **_engine_kwargs)
SessionLocal = sessionmaker(bind=engine, autoflush=False, autocommit=False)

Base = declarative_base()


def _utcnow() -> datetime:
    """Timezone-aware UTC timestamp helper, used for server-side
    received_at columns across all tables."""
    return datetime.now(timezone.utc)


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------

class Device(Base):
    """A physical device (vehicle) that has reported at least one
    message. Rows are created/updated automatically by mqtt_handler.py
    whenever a message arrives from a device_id not yet seen, or to
    refresh last_seen_at on every message."""

    __tablename__ = "devices"

    device_id = Column(String(32), primary_key=True)
    device_type = Column(String(64), nullable=True)
    firmware_version = Column(String(16), nullable=True)
    first_seen_at = Column(DateTime(timezone=True), nullable=False, default=_utcnow)
    last_seen_at = Column(DateTime(timezone=True), nullable=False, default=_utcnow)


class TelemetryRecord(Base):
    """One raw fused IMU + GPS + OBD-II snapshot, as published to
    fleet/{device_id}/telemetry."""

    __tablename__ = "telemetry_records"

    id = Column(Integer, primary_key=True, autoincrement=True)
    device_id = Column(String(32), ForeignKey("devices.device_id"), nullable=False, index=True)
    timestamp_ms = Column(BigInteger, nullable=False)

    accel_x_g = Column(Float, nullable=True)
    accel_y_g = Column(Float, nullable=True)
    accel_z_g = Column(Float, nullable=True)
    gyro_x_dps = Column(Float, nullable=True)
    gyro_y_dps = Column(Float, nullable=True)
    gyro_z_dps = Column(Float, nullable=True)

    gps_fix_valid = Column(Boolean, nullable=True)
    latitude_deg = Column(Float, nullable=True)
    longitude_deg = Column(Float, nullable=True)
    gps_speed_kmh = Column(Float, nullable=True)
    heading_deg = Column(Float, nullable=True)

    engine_rpm_valid = Column(Boolean, nullable=True)
    engine_rpm = Column(Integer, nullable=True)
    obd_speed_valid = Column(Boolean, nullable=True)
    obd_speed_kmh = Column(Float, nullable=True)
    throttle_position_valid = Column(Boolean, nullable=True)
    throttle_position_pct = Column(Float, nullable=True)

    received_at = Column(DateTime(timezone=True), nullable=False, default=_utcnow)

    __table_args__ = (
        Index("ix_telemetry_device_ts", "device_id", "timestamp_ms"),
    )


class ScoreRecord(Base):
    """One driver safety score snapshot, as published to
    fleet/{device_id}/score."""

    __tablename__ = "score_records"

    id = Column(Integer, primary_key=True, autoincrement=True)
    device_id = Column(String(32), ForeignKey("devices.device_id"), nullable=False, index=True)
    timestamp_ms = Column(BigInteger, nullable=False)

    score = Column(Float, nullable=False)
    harsh_braking_count = Column(Integer, nullable=False, default=0)
    harsh_accel_count = Column(Integer, nullable=False, default=0)
    harsh_cornering_count = Column(Integer, nullable=False, default=0)
    overspeed_count = Column(Integer, nullable=False, default=0)
    idling_seconds_total = Column(Float, nullable=False, default=0.0)
    crash_count = Column(Integer, nullable=False, default=0)
    geofence_violation_count = Column(Integer, nullable=False, default=0)

    received_at = Column(DateTime(timezone=True), nullable=False, default=_utcnow)

    __table_args__ = (
        Index("ix_score_device_ts", "device_id", "timestamp_ms"),
    )


class AlertRecord(Base):
    """One non-crash safety alert (harsh event summary is NOT sent as an
    alert - only geofence violations and excessive idling are, per the
    firmware's driver_score/geofence/idling_detection design), as
    published to fleet/{device_id}/alert.

    The alert schema varies by alert_type (geofence violations carry
    zone_name/event/zone_type; idling alerts carry duration_seconds), so
    rather than modeling every variant as its own set of nullable
    columns, the full original JSON payload is preserved in
    raw_payload_json for completeness, with the handful of fields common
    or most useful across alert types promoted to real columns for
    querying/filtering.
    """

    __tablename__ = "alert_records"

    id = Column(Integer, primary_key=True, autoincrement=True)
    device_id = Column(String(32), ForeignKey("devices.device_id"), nullable=False, index=True)
    timestamp_ms = Column(BigInteger, nullable=False)

    alert_type = Column(String(64), nullable=False, index=True)
    latitude_deg = Column(Float, nullable=True)
    longitude_deg = Column(Float, nullable=True)
    raw_payload_json = Column(Text, nullable=False)

    received_at = Column(DateTime(timezone=True), nullable=False, default=_utcnow)

    __table_args__ = (
        Index("ix_alert_device_ts", "device_id", "timestamp_ms"),
    )


class CrashRecord(Base):
    """One confirmed crash event, as published to
    fleet/{device_id}/crash. This is the highest-severity table; a
    fleet dashboard should surface these prominently."""

    __tablename__ = "crash_records"

    id = Column(Integer, primary_key=True, autoincrement=True)
    device_id = Column(String(32), ForeignKey("devices.device_id"), nullable=False, index=True)
    timestamp_ms = Column(BigInteger, nullable=False)

    total_accel_g = Column(Float, nullable=False)
    total_gyro_dps = Column(Float, nullable=False)
    accel_x_g = Column(Float, nullable=True)
    accel_y_g = Column(Float, nullable=True)
    accel_z_g = Column(Float, nullable=True)
    gyro_x_dps = Column(Float, nullable=True)
    gyro_y_dps = Column(Float, nullable=True)
    gyro_z_dps = Column(Float, nullable=True)

    gps_fix_valid = Column(Boolean, nullable=True)
    latitude_deg = Column(Float, nullable=True)
    longitude_deg = Column(Float, nullable=True)
    speed_kmh = Column(Float, nullable=True)
    heading_deg = Column(Float, nullable=True)

    received_at = Column(DateTime(timezone=True), nullable=False, default=_utcnow)

    __table_args__ = (
        Index("ix_crash_device_ts", "device_id", "timestamp_ms"),
    )


class StatusRecord(Base):
    """One device health/status snapshot, as published to
    fleet/{device_id}/status."""

    __tablename__ = "status_records"

    id = Column(Integer, primary_key=True, autoincrement=True)
    device_id = Column(String(32), ForeignKey("devices.device_id"), nullable=False, index=True)
    timestamp_ms = Column(BigInteger, nullable=False)

    uptime_sec = Column(BigInteger, nullable=True)
    free_heap_bytes = Column(Integer, nullable=True)
    min_free_heap_bytes = Column(Integer, nullable=True)
    wifi_connected = Column(Boolean, nullable=True)
    mqtt_connected = Column(Boolean, nullable=True)
    wifi_rssi_dbm = Column(Integer, nullable=True)
    current_driver_score = Column(Float, nullable=True)

    received_at = Column(DateTime(timezone=True), nullable=False, default=_utcnow)

    __table_args__ = (
        Index("ix_status_device_ts", "device_id", "timestamp_ms"),
    )


# ---------------------------------------------------------------------------
# Session / Schema Management
# ---------------------------------------------------------------------------

def init_db() -> None:
    """Create all tables if they do not already exist. Safe to call on
    every application startup; does not drop or modify existing tables,
    so it is not a substitute for a real migration tool (e.g. Alembic)
    if the schema changes after data has been collected."""
    Base.metadata.create_all(bind=engine)
    logger.info("Database schema initialized (%s)", DATABASE_URL)


def get_session() -> Session:
    """Create a new SQLAlchemy session. Callers are responsible for
    closing it (typically via a 'with' block or try/finally), and for
    committing/rolling back their own transactions."""
    return SessionLocal()


# ---------------------------------------------------------------------------
# Insert Helpers
#
# These are used by mqtt_handler.py to persist incoming messages. Each
# function takes an already-open Session (so the caller controls
# transaction boundaries and can batch multiple inserts) and the
# device_id plus a dict of already-validated fields.
# ---------------------------------------------------------------------------

def upsert_device(session: Session, device_id: str, device_type: Optional[str] = None,
                   firmware_version: Optional[str] = None) -> None:
    """Insert a new Device row, or update last_seen_at (and
    device_type/firmware_version, if provided) on an existing one."""
    device = session.get(Device, device_id)
    now = _utcnow()

    if device is None:
        device = Device(
            device_id=device_id,
            device_type=device_type,
            firmware_version=firmware_version,
            first_seen_at=now,
            last_seen_at=now,
        )
        session.add(device)
        logger.info("New device registered: %s", device_id)
    else:
        device.last_seen_at = now
        if device_type is not None:
            device.device_type = device_type
        if firmware_version is not None:
            device.firmware_version = firmware_version


def insert_telemetry(session: Session, device_id: str, payload: dict) -> None:
    record = TelemetryRecord(
        device_id=device_id,
        timestamp_ms=payload.get("timestamp_ms", 0),
        accel_x_g=payload.get("accel_x_g"),
        accel_y_g=payload.get("accel_y_g"),
        accel_z_g=payload.get("accel_z_g"),
        gyro_x_dps=payload.get("gyro_x_dps"),
        gyro_y_dps=payload.get("gyro_y_dps"),
        gyro_z_dps=payload.get("gyro_z_dps"),
        gps_fix_valid=payload.get("gps_fix_valid"),
        latitude_deg=payload.get("latitude_deg"),
        longitude_deg=payload.get("longitude_deg"),
        gps_speed_kmh=payload.get("gps_speed_kmh"),
        heading_deg=payload.get("heading_deg"),
        engine_rpm_valid=payload.get("engine_rpm_valid"),
        engine_rpm=payload.get("engine_rpm"),
        obd_speed_valid=payload.get("obd_speed_valid"),
        obd_speed_kmh=payload.get("obd_speed_kmh"),
        throttle_position_valid=payload.get("throttle_position_valid"),
        throttle_position_pct=payload.get("throttle_position_pct"),
    )
    session.add(record)


def insert_score(session: Session, device_id: str, payload: dict) -> None:
    record = ScoreRecord(
        device_id=device_id,
        timestamp_ms=payload.get("timestamp_ms", 0),
        score=payload.get("score", 0.0),
        harsh_braking_count=payload.get("harsh_braking_count", 0),
        harsh_accel_count=payload.get("harsh_accel_count", 0),
        harsh_cornering_count=payload.get("harsh_cornering_count", 0),
        overspeed_count=payload.get("overspeed_count", 0),
        idling_seconds_total=payload.get("idling_seconds_total", 0.0),
        crash_count=payload.get("crash_count", 0),
        geofence_violation_count=payload.get("geofence_violation_count", 0),
    )
    session.add(record)


def insert_alert(session: Session, device_id: str, payload: dict, raw_payload_json: str) -> None:
    record = AlertRecord(
        device_id=device_id,
        timestamp_ms=payload.get("timestamp_ms", 0),
        alert_type=payload.get("alert_type", "unknown"),
        latitude_deg=payload.get("latitude_deg"),
        longitude_deg=payload.get("longitude_deg"),
        raw_payload_json=raw_payload_json,
    )
    session.add(record)


def insert_crash(session: Session, device_id: str, payload: dict) -> None:
    record = CrashRecord(
        device_id=device_id,
        timestamp_ms=payload.get("timestamp_ms", 0),
        total_accel_g=payload.get("total_accel_g", 0.0),
        total_gyro_dps=payload.get("total_gyro_dps", 0.0),
        accel_x_g=payload.get("accel_x_g"),
        accel_y_g=payload.get("accel_y_g"),
        accel_z_g=payload.get("accel_z_g"),
        gyro_x_dps=payload.get("gyro_x_dps"),
        gyro_y_dps=payload.get("gyro_y_dps"),
        gyro_z_dps=payload.get("gyro_z_dps"),
        gps_fix_valid=payload.get("gps_fix_valid"),
        latitude_deg=payload.get("latitude_deg"),
        longitude_deg=payload.get("longitude_deg"),
        speed_kmh=payload.get("speed_kmh"),
        heading_deg=payload.get("heading_deg"),
    )
    session.add(record)


def insert_status(session: Session, device_id: str, payload: dict) -> None:
    record = StatusRecord(
        device_id=device_id,
        timestamp_ms=payload.get("timestamp_ms", 0),
        uptime_sec=payload.get("uptime_sec"),
        free_heap_bytes=payload.get("free_heap_bytes"),
        min_free_heap_bytes=payload.get("min_free_heap_bytes"),
        wifi_connected=payload.get("wifi_connected"),
        mqtt_connected=payload.get("mqtt_connected"),
        wifi_rssi_dbm=payload.get("wifi_rssi_dbm"),
        current_driver_score=payload.get("current_driver_score"),
    )
    session.add(record)
