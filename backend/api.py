"""
api.py

REST API router for the Driver Behaviour Analysis System backend.

Exposes read-only endpoints consumed primarily by the Node-RED dashboard
(dashboard/flow.json) and any other downstream consumer.

IMPORTANT SCHEMA NOTE
----------------------
This module was previously written against a Driver/Trip/SensorReading
schema that does not exist anywhere in this codebase - the firmware
publishes per-device telemetry/score/alert/crash/status messages keyed
only by device_id (a MAC-derived hex string), and mqtt_handler.py writes
exactly that shape into database.py's actual tables: Device,
TelemetryRecord, ScoreRecord, AlertRecord, CrashRecord, StatusRecord.
There is no "driver" or "trip" entity produced anywhere in this system.

This rewrite queries the schema that is actually populated. The current
dashboard/flow.json build calls:
    GET /api/v1/devices/{deviceId}/score
    GET /api/v1/devices/{deviceId}/alerts?limit=10
and parses msg.payload.score / msg.payload.rating for the score gauge,
and item.timestamp_ms / item.alert_type / item.latitude_deg /
item.longitude_deg / item.raw_payload_json for the alerts table. The
routes and response models below match that contract exactly. The
original /drivers/{device_id}/score and /drivers/{device_id}/alerts
paths are kept as backward-compatible aliases (same handler, same
response shape) in case any other consumer still targets them, but
flow.json itself now hits the /devices/... paths.

Write paths into the underlying tables are intentionally excluded from
this router: telemetry/score/alert/crash/status rows are written
exclusively by mqtt_handler.py as MQTT messages arrive from fleet
devices. Keeping a single writer per table avoids race conditions and
keeps this API's contract purely query-oriented.
"""

from __future__ import annotations

import json
import logging
from datetime import datetime
from typing import Generator, List, Optional

from fastapi import APIRouter, Depends, HTTPException, Query, status
from pydantic import BaseModel, ConfigDict
from sqlalchemy import desc
from sqlalchemy.exc import SQLAlchemyError
from sqlalchemy.orm import Session

from database import (
    Device,
    TelemetryRecord,
    ScoreRecord,
    AlertRecord,
    CrashRecord,
    StatusRecord,
    get_session,
)

logger = logging.getLogger("api")

router = APIRouter()

# Hard upper bound on page size to protect the database/network from
# accidental or malicious unbounded queries against telemetry tables,
# which can grow to tens of millions of rows for an active fleet.
MAX_PAGE_SIZE = 500
DEFAULT_PAGE_SIZE = 50


# --------------------------------------------------------------------------
# Dependency: per-request database session
# --------------------------------------------------------------------------
def get_db() -> Generator[Session, None, None]:
    """
    FastAPI dependency that yields a database session scoped to a
    single request. SQLAlchemy's Session object is itself a context
    manager (closes on exit); get_session() just returns a plain
    Session instance, which the 'with' block below manages correctly.
    """
    with get_session() as session:
        yield session


# --------------------------------------------------------------------------
# Response schemas
# --------------------------------------------------------------------------
class DeviceOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    device_id: str
    device_type: Optional[str] = None
    firmware_version: Optional[str] = None
    first_seen_at: datetime
    last_seen_at: datetime


class TelemetryOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    device_id: str
    timestamp_ms: int

    accel_x_g: Optional[float] = None
    accel_y_g: Optional[float] = None
    accel_z_g: Optional[float] = None
    gyro_x_dps: Optional[float] = None
    gyro_y_dps: Optional[float] = None
    gyro_z_dps: Optional[float] = None

    gps_fix_valid: Optional[bool] = None
    latitude_deg: Optional[float] = None
    longitude_deg: Optional[float] = None
    gps_speed_kmh: Optional[float] = None
    heading_deg: Optional[float] = None

    engine_rpm_valid: Optional[bool] = None
    engine_rpm: Optional[int] = None
    obd_speed_valid: Optional[bool] = None
    obd_speed_kmh: Optional[float] = None
    throttle_position_valid: Optional[bool] = None
    throttle_position_pct: Optional[float] = None


class ScoreOut(BaseModel):
    """
    Dashboard-compatible score payload. dashboard/flow.json's
    'Format Safety Score' function node reads msg.payload.score and
    msg.payload.rating directly, so this response uses the same field
    name as the underlying ScoreRecord.score column - no renaming - plus
    the server-computed 'rating' bucket the dashboard also expects.
    """

    device_id: str
    timestamp_ms: int
    score: float
    rating: str
    harsh_braking_count: int
    harsh_accel_count: int
    harsh_cornering_count: int
    overspeed_count: int
    idling_seconds_total: float
    crash_count: int
    geofence_violation_count: int


class AlertOut(BaseModel):
    """
    Dashboard-compatible alert payload. dashboard/flow.json's
    'Format Alerts Table' function node reads item.timestamp_ms,
    item.alert_type, item.latitude_deg, item.longitude_deg, and
    item.raw_payload_json (rendered into the "Details" column) - so
    those are the field names used here, matching AlertRecord's own
    columns 1:1. severity/message are kept as additional derived fields
    for any other consumer that wants a human-readable summary, but the
    dashboard itself does not read them.
    """

    timestamp_ms: int
    alert_type: str
    raw_payload_json: str
    severity: str
    message: str
    latitude_deg: Optional[float] = None
    longitude_deg: Optional[float] = None


class CrashOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    device_id: str
    timestamp_ms: int
    total_accel_g: float
    total_gyro_dps: float
    latitude_deg: Optional[float] = None
    longitude_deg: Optional[float] = None
    speed_kmh: Optional[float] = None


class StatusOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    device_id: str
    timestamp_ms: int
    uptime_sec: Optional[int] = None
    free_heap_bytes: Optional[int] = None
    min_free_heap_bytes: Optional[int] = None
    wifi_connected: Optional[bool] = None
    mqtt_connected: Optional[bool] = None
    wifi_rssi_dbm: Optional[int] = None
    current_driver_score: Optional[float] = None


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------
def _run_query(db: Session, description: str, fn):
    """
    Executes a query callable, translating SQLAlchemy failures into a
    uniform 500 response while logging full detail server-side.
    """
    try:
        return fn()
    except SQLAlchemyError:
        logger.exception("Database query failed: %s", description)
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail="A database error occurred while processing the request.",
        )


def _paging(limit: int, offset: int) -> tuple[int, int]:
    """
    Validates and clamps pagination parameters. Raises HTTP 400 for
    negative offsets rather than silently coercing them.
    """
    if offset < 0:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="offset must be >= 0.",
        )
    return min(limit, MAX_PAGE_SIZE), offset


def _rating_for_score(score: float) -> str:
    """
    Buckets a numeric score into the same tiers dashboard/flow.json's
    gauge already visualizes (seg1=50, seg2=80 on node_gauge_score).
    """
    if score >= 80:
        return "Excellent"
    if score >= 50:
        return "Fair"
    return "Poor"


def _severity_and_message_for_alert(record: AlertRecord) -> tuple[str, str]:
    """
    Derives a human-readable severity/message pair from an AlertRecord.
    Only two alert_type values are ever published by the firmware:
    "excessive_idling" (idling_detection.c) and "geofence_violation"
    (geofence.c). Crashes are a separate, higher-severity MQTT topic
    and table (CrashRecord) entirely, not part of AlertRecord.
    """
    try:
        payload = json.loads(record.raw_payload_json)
    except (json.JSONDecodeError, TypeError):
        payload = {}

    if record.alert_type == "geofence_violation":
        zone_name = payload.get("zone_name", "unknown zone")
        event = payload.get("event", "transitioned")
        return "High", f"Vehicle {event} geofence zone \"{zone_name}\""

    if record.alert_type == "excessive_idling":
        duration = payload.get("duration_seconds")
        duration_str = f"{duration:.0f}s" if isinstance(duration, (int, float)) else "unknown duration"
        return "Medium", f"Excessive idling detected ({duration_str})"

    # The firmware only ever publishes the two alert_type values handled
    # above (idling_detection.c, geofence.c); anything else is unexpected
    # and worth surfacing in logs for debugging, though it shouldn't break
    # the response.
    logger.warning("Unexpected alert_type '%s' from device %s", record.alert_type, record.device_id)
    return "Low", f"Alert: {record.alert_type}"


def _get_device_or_404(db: Session, device_id: str) -> Device:
    device = db.query(Device).filter(Device.device_id == device_id).one_or_none()
    if device is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Device '{device_id}' not found.",
        )
    return device


# --------------------------------------------------------------------------
# Devices
# --------------------------------------------------------------------------
@router.get("/devices", response_model=List[DeviceOut], tags=["devices"])
def list_devices(
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[Device]:
    """Returns a paginated list of devices that have reported at least
    one message, most recently seen first."""
    limit, offset = _paging(limit, offset)

    def query():
        return (
            db.query(Device)
            .order_by(desc(Device.last_seen_at))
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "list_devices", query)


@router.get("/devices/{device_id}", response_model=DeviceOut, tags=["devices"])
def get_device(device_id: str, db: Session = Depends(get_db)) -> Device:
    """Returns a single device by id, or 404 if it has never reported."""

    def query():
        return _get_device_or_404(db, device_id)

    return _run_query(db, "get_device", query)


# --------------------------------------------------------------------------
# Telemetry
# --------------------------------------------------------------------------
@router.get(
    "/devices/{device_id}/telemetry",
    response_model=List[TelemetryOut],
    tags=["telemetry"],
)
def get_device_telemetry(
    device_id: str,
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[TelemetryRecord]:
    """Returns historical telemetry for a device, most recent first."""
    limit, offset = _paging(limit, offset)

    def query():
        _get_device_or_404(db, device_id)
        return (
            db.query(TelemetryRecord)
            .filter(TelemetryRecord.device_id == device_id)
            .order_by(desc(TelemetryRecord.timestamp_ms))
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "get_device_telemetry", query)


@router.get(
    "/devices/{device_id}/telemetry/latest",
    response_model=List[TelemetryOut],
    tags=["telemetry"],
)
def get_device_latest_telemetry(
    device_id: str,
    limit: int = Query(20, ge=1, le=MAX_PAGE_SIZE),
    db: Session = Depends(get_db),
) -> List[TelemetryRecord]:
    """Returns the most recent `limit` telemetry rows for a device,
    newest first - intended for near-real-time dashboard polling."""

    def query():
        _get_device_or_404(db, device_id)
        return (
            db.query(TelemetryRecord)
            .filter(TelemetryRecord.device_id == device_id)
            .order_by(desc(TelemetryRecord.timestamp_ms))
            .limit(limit)
            .all()
        )

    return _run_query(db, "get_device_latest_telemetry", query)


# --------------------------------------------------------------------------
# Scores
#
# Routes are kept under /drivers/... to match dashboard/flow.json's
# existing HTTP request nodes without requiring a flow edit; the path
# parameter is a device_id, not a separate driver entity (see module
# docstring).
# --------------------------------------------------------------------------
@router.get("/devices/{device_id}/score", response_model=ScoreOut, tags=["scores"])
@router.get(
    "/drivers/{device_id}/score",
    response_model=ScoreOut,
    tags=["scores"],
    include_in_schema=False,  # legacy alias, kept for backward compatibility
)
def get_latest_score(device_id: str, db: Session = Depends(get_db)) -> ScoreOut:
    """Returns the most recent safety score for a device, shaped for
    dashboard/flow.json's gauge and rating text widgets."""

    def query() -> ScoreOut:
        _get_device_or_404(db, device_id)
        record = (
            db.query(ScoreRecord)
            .filter(ScoreRecord.device_id == device_id)
            .order_by(desc(ScoreRecord.timestamp_ms))
            .first()
        )
        if record is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"No score has been published yet for device '{device_id}'.",
            )
        return ScoreOut(
            device_id=record.device_id,
            timestamp_ms=record.timestamp_ms,
            score=record.score,
            rating=_rating_for_score(record.score),
            harsh_braking_count=record.harsh_braking_count,
            harsh_accel_count=record.harsh_accel_count,
            harsh_cornering_count=record.harsh_cornering_count,
            overspeed_count=record.overspeed_count,
            idling_seconds_total=record.idling_seconds_total,
            crash_count=record.crash_count,
            geofence_violation_count=record.geofence_violation_count,
        )

    return _run_query(db, "get_latest_score", query)


@router.get(
    "/devices/{device_id}/score/history",
    response_model=List[ScoreOut],
    tags=["scores"],
)
def get_score_history(
    device_id: str,
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[ScoreOut]:
    """Returns a device's historical safety scores, most recent first -
    intended to feed a trend chart."""
    limit, offset = _paging(limit, offset)

    def query() -> List[ScoreOut]:
        _get_device_or_404(db, device_id)
        records = (
            db.query(ScoreRecord)
            .filter(ScoreRecord.device_id == device_id)
            .order_by(desc(ScoreRecord.timestamp_ms))
            .offset(offset)
            .limit(limit)
            .all()
        )
        return [
            ScoreOut(
                device_id=r.device_id,
                timestamp_ms=r.timestamp_ms,
                score=r.score,
                rating=_rating_for_score(r.score),
                harsh_braking_count=r.harsh_braking_count,
                harsh_accel_count=r.harsh_accel_count,
                harsh_cornering_count=r.harsh_cornering_count,
                overspeed_count=r.overspeed_count,
                idling_seconds_total=r.idling_seconds_total,
                crash_count=r.crash_count,
                geofence_violation_count=r.geofence_violation_count,
            )
            for r in records
        ]

    return _run_query(db, "get_score_history", query)


# --------------------------------------------------------------------------
# Alerts
#
# Also kept under /drivers/... for the same dashboard-compatibility
# reason as the score route above.
# --------------------------------------------------------------------------
@router.get("/devices/{device_id}/alerts", response_model=List[AlertOut], tags=["alerts"])
@router.get(
    "/drivers/{device_id}/alerts",
    response_model=List[AlertOut],
    tags=["alerts"],
    include_in_schema=False,  # legacy alias, kept for backward compatibility
)
def get_device_alerts(
    device_id: str,
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[AlertOut]:
    """Returns alerts (idling/geofence) for a device, most recent first,
    shaped for dashboard/flow.json's alerts table widget."""
    limit, offset = _paging(limit, offset)

    def query() -> List[AlertOut]:
        _get_device_or_404(db, device_id)
        records = (
            db.query(AlertRecord)
            .filter(AlertRecord.device_id == device_id)
            .order_by(desc(AlertRecord.timestamp_ms))
            .offset(offset)
            .limit(limit)
            .all()
        )
        result = []
        for r in records:
            severity, message = _severity_and_message_for_alert(r)
            result.append(
                AlertOut(
                    timestamp_ms=r.timestamp_ms,
                    alert_type=r.alert_type,
                    raw_payload_json=r.raw_payload_json,
                    severity=severity,
                    message=message,
                    latitude_deg=r.latitude_deg,
                    longitude_deg=r.longitude_deg,
                )
            )
        return result

    return _run_query(db, "get_device_alerts", query)


@router.get("/alerts", response_model=List[AlertOut], tags=["alerts"])
def list_alerts(
    device_id: Optional[str] = Query(None, description="Filter by device id."),
    alert_type: Optional[str] = Query(
        None, description="Filter by alert type: excessive_idling or geofence_violation."
    ),
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[AlertOut]:
    """Returns fleet-wide alerts, most recent first, optionally filtered
    by device and/or alert type - a fleet-ops "recent incidents" feed."""
    limit, offset = _paging(limit, offset)

    def query() -> List[AlertOut]:
        q = db.query(AlertRecord)
        if device_id is not None:
            q = q.filter(AlertRecord.device_id == device_id)
        if alert_type is not None:
            q = q.filter(AlertRecord.alert_type == alert_type)
        records = q.order_by(desc(AlertRecord.timestamp_ms)).offset(offset).limit(limit).all()

        result = []
        for r in records:
            severity, message = _severity_and_message_for_alert(r)
            result.append(
                AlertOut(
                    timestamp_ms=r.timestamp_ms,
                    alert_type=r.alert_type,
                    raw_payload_json=r.raw_payload_json,
                    severity=severity,
                    message=message,
                    latitude_deg=r.latitude_deg,
                    longitude_deg=r.longitude_deg,
                )
            )
        return result

    return _run_query(db, "list_alerts", query)


# --------------------------------------------------------------------------
# Crashes
#
# Crashes are the highest-severity event type and deliberately kept in
# their own table/endpoint rather than folded into /alerts, mirroring
# how mqtt_client.c/crash_detection.c treat them as a distinct,
# highest-QoS message type.
# --------------------------------------------------------------------------
@router.get(
    "/devices/{device_id}/crashes",
    response_model=List[CrashOut],
    tags=["crashes"],
)
def get_device_crashes(
    device_id: str,
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[CrashRecord]:
    """Returns confirmed crash events for a device, most recent first."""
    limit, offset = _paging(limit, offset)

    def query():
        _get_device_or_404(db, device_id)
        return (
            db.query(CrashRecord)
            .filter(CrashRecord.device_id == device_id)
            .order_by(desc(CrashRecord.timestamp_ms))
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "get_device_crashes", query)


# --------------------------------------------------------------------------
# Status
# --------------------------------------------------------------------------
@router.get(
    "/devices/{device_id}/status",
    response_model=StatusOut,
    tags=["status"],
)
def get_device_latest_status(device_id: str, db: Session = Depends(get_db)) -> StatusRecord:
    """Returns the most recent health/status snapshot for a device
    (uptime, heap, WiFi/MQTT connectivity, RSSI, current score)."""

    def query():
        _get_device_or_404(db, device_id)
        record = (
            db.query(StatusRecord)
            .filter(StatusRecord.device_id == device_id)
            .order_by(desc(StatusRecord.timestamp_ms))
            .first()
        )
        if record is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"No status has been published yet for device '{device_id}'.",
            )
        return record

    return _run_query(db, "get_device_latest_status", query)
