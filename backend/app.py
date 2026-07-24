"""
app.py

Main entry point for the Driver Behaviour Analysis System backend.

Responsibilities:
    - Bootstrap the FastAPI application.
    - Initialize the database connection/schema on startup.
    - Start the MQTT ingestion handler as a background thread so that
      telemetry published by ESP32 fleet devices is continuously
      persisted while the REST API concurrently serves dashboard/API
      consumers.
    - Expose a health-check endpoint for uptime monitoring (e.g. by
      Node-RED, load balancers, or container orchestrators).
    - Mount the REST API router defined in api.py.

Design notes / assumptions (stated explicitly per project requirements,
since database.py and mqtt_handler.py are not being generated in this
exchange but app.py depends on them):

    1. FastAPI was selected over Flask because:
         - Native async support is required so the synchronous MQTT
           client thread never blocks HTTP request handling.
         - Built-in request/response validation via Pydantic reduces
           malformed-payload bugs when a fleet of embedded devices is
           writing to the API.
         - Automatic OpenAPI docs generation aids operational/fleet
           debugging.
    2. database.py is assumed to expose the following interface:
           - init_db() -> None
                 Creates tables if they do not already exist.
           - get_session() -> contextlib.AbstractContextManager[Session]
                 Context manager yielding a SQLAlchemy Session.
           - engine
                 The SQLAlchemy Engine instance (used for health checks).
       These names form a fixed contract; api.py and mqtt_handler.py
       must conform to it.
    3. mqtt_handler.py is assumed to expose:
           - class MQTTIngestHandler:
                 def __init__(self, broker_host: str, broker_port: int,
                              topic_prefix: str, client_id: str) -> None
                 def start(self) -> None    # non-blocking, spawns its own thread
                 def stop(self) -> None     # graceful disconnect, flushes QoS 1/2
                 def is_connected(self) -> bool
       Running the MQTT client on its own dedicated thread (rather than
       inside the asyncio event loop) avoids coupling the paho-mqtt
       network loop to FastAPI's event loop, so a slow/unreachable
       broker can never stall HTTP request handling.
    4. Configuration is sourced from environment variables via
       pydantic-settings, with defaults overridable by a local .env
       file. config.py was not requested in this exchange, so Settings
       are defined locally here; if a shared config.py is introduced
       later, this class should move there unchanged.
    5. CORS is enabled for all origins ONLY by default, because the
       Node-RED dashboard may be served from a different host/port on
       the fleet operator's internal network. This MUST be restricted
       to explicit origins (CORS_ORIGINS env var) before exposure
       beyond a trusted internal network.
    6. api.py is assumed to expose `router: fastapi.APIRouter`.

Run (development):
    uvicorn app:app --reload --host 0.0.0.0 --port 8000

Run (production):
    uvicorn app:app --host 0.0.0.0 --port 8000 --workers 1
    NOTE: If scaling beyond 1 worker process, only one process should
    run the MQTT ingestion handler (e.g. via a dedicated ingestion
    process/deployment) to avoid duplicate message processing; the
    MQTT_CLIENT_ID_SUFFIX below exists to keep client IDs unique if
    multiple ingestion-enabled workers are ever run intentionally.
"""

from __future__ import annotations

import logging
import sys
import uuid
from contextlib import asynccontextmanager
from typing import AsyncIterator, List

from fastapi import FastAPI, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic_settings import BaseSettings, SettingsConfigDict
from sqlalchemy import text
from sqlalchemy.exc import SQLAlchemyError

from database import engine, init_db
from mqtt_handler import MQTTIngestHandler
from api import router as api_router


# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------
class Settings(BaseSettings):
    """
    Centralized runtime configuration, loaded from environment
    variables (and an optional .env file for local development).

    All fields have production-safe defaults EXCEPT cors_origins,
    which defaults to '*' for developer convenience and must be
    overridden in production via the CORS_ORIGINS environment
    variable (comma-separated list of allowed origins).
    """

    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8")

    app_name: str = "Driver Behaviour Analysis System API"
    app_version: str = "1.0.0"

    mqtt_broker_host: str = "localhost"
    mqtt_broker_port: int = 1883
    mqtt_topic_prefix: str = "fleet/telemetry"
    # Distinguishes MQTT client IDs across processes/restarts so the
    # broker never rejects a duplicate persistent client ID.
    mqtt_client_id_suffix: str = uuid.uuid4().hex[:8]

    cors_origins: str = "*"

    log_level: str = "INFO"


settings = Settings()


# --------------------------------------------------------------------------
# Logging
# --------------------------------------------------------------------------
logging.basicConfig(
    level=getattr(logging, settings.log_level.upper(), logging.INFO),
    format="%(asctime)s | %(levelname)-8s | %(name)s | %(message)s",
    handlers=[logging.StreamHandler(sys.stdout)],
)
logger = logging.getLogger("app")


# --------------------------------------------------------------------------
# MQTT handler instance.
#
# A module-level singleton is intentional here: there must be exactly
# one MQTT ingestion connection per running process, and it must be
# reachable from both the lifespan startup/shutdown hooks and the
# /health endpoint.
# --------------------------------------------------------------------------
mqtt_handler = MQTTIngestHandler(
    broker_host=settings.mqtt_broker_host,
    broker_port=settings.mqtt_broker_port,
    topic_prefix=settings.mqtt_topic_prefix,
    client_id=f"dbas-backend-{settings.mqtt_client_id_suffix}",
)


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    """
    Application lifespan handler.

    Startup:
        - Initialize database schema.
        - Start the MQTT ingestion handler on its own background thread.
    Shutdown:
        - Gracefully disconnect the MQTT handler so in-flight QoS 1/2
          messages are acknowledged before the process exits.

    Any failure during startup is re-raised so the process fails fast
    rather than serving traffic against a half-initialized backend --
    this is deliberate for a system whose data feeds a safety score.
    """
    logger.info("Starting %s v%s", settings.app_name, settings.app_version)

    try:
        init_db()
        logger.info("Database schema initialized successfully.")
    except SQLAlchemyError:
        logger.exception("Failed to initialize database schema. Aborting startup.")
        raise

    try:
        mqtt_handler.start()
        logger.info(
            "MQTT ingestion handler started (broker=%s:%d, topic_prefix=%s).",
            settings.mqtt_broker_host,
            settings.mqtt_broker_port,
            settings.mqtt_topic_prefix,
        )
    except Exception:
        logger.exception("Failed to start MQTT ingestion handler. Aborting startup.")
        raise

    yield

    logger.info("Shutting down %s.", settings.app_name)
    try:
        mqtt_handler.stop()
        logger.info("MQTT ingestion handler stopped cleanly.")
    except Exception:
        # Shutdown-path failures are logged, not re-raised: we do not
        # want a teardown error to mask the original shutdown signal
        # or prevent the process from exiting.
        logger.exception("Error while stopping MQTT ingestion handler.")


# --------------------------------------------------------------------------
# FastAPI application
# --------------------------------------------------------------------------
app = FastAPI(
    title=settings.app_name,
    version=settings.app_version,
    description=(
        "Backend service for the Driver Behaviour Analysis System. "
        "Ingests vehicle telemetry from ESP32 fleet devices over MQTT, "
        "persists it, and exposes REST endpoints consumed by the "
        "Node-RED dashboard and the ML scoring pipeline."
    ),
    lifespan=lifespan,
)

_allowed_origins: List[str] = [o.strip() for o in settings.cors_origins.split(",") if o.strip()]

app.add_middleware(
    CORSMiddleware,
    allow_origins=_allowed_origins,
    allow_credentials=True,
    allow_methods=["GET", "POST"],
    allow_headers=["*"],
)

app.include_router(api_router, prefix="/api/v1")


@app.get("/health", status_code=status.HTTP_200_OK, tags=["monitoring"])
async def health_check() -> JSONResponse:
    """
    Liveness/readiness probe.

    Returns HTTP 200 with per-component status when both the database
    and the MQTT ingestion handler are reachable; otherwise returns
    HTTP 503 so upstream load balancers/orchestrators can react (e.g.
    stop routing traffic, restart the container).
    """
    db_ok = False
    try:
        with engine.connect() as conn:
            conn.execute(text("SELECT 1"))
        db_ok = True
    except SQLAlchemyError:
        logger.exception("Health check: database connectivity failed.")

    mqtt_ok = False
    try:
        mqtt_ok = mqtt_handler.is_connected()
    except Exception:
        logger.exception("Health check: MQTT status query failed.")

    body = {
        "service": settings.app_name,
        "version": settings.app_version,
        "database": "up" if db_ok else "down",
        "mqtt": "up" if mqtt_ok else "down",
    }

    overall_status = (
        status.HTTP_200_OK
        if (db_ok and mqtt_ok)
        else status.HTTP_503_SERVICE_UNAVAILABLE
    )
    return JSONResponse(status_code=overall_status, content=body)
