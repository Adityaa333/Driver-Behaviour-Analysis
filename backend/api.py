"""
api.py

REST API router for the Driver Behaviour Analysis System backend.

Exposes read-only endpoints consumed primarily by the Node-RED
dashboard to query devices, raw telemetry, computed driver safety
scores, alerts, crashes, and status for each fleet device.

FIX NOTE (see accompanying compatibility review): the previous version
of this file imported `Alert, Driver, ScoreRecord, SensorReading, Trip`
from database.py and modeled a driver/trip-centric schema. None of
those names exist in database.py, which is device_id-keyed and mirrors
the five MQTT payload types the firmware actually publishes
(fleet/{device_id}/telemetry|score|alert|crash|status). This version
is rewritten against that real schema so the import succeeds and the
response fields match what's actually stored.

All write paths are intentionally excluded from this router; writes
happen exclusively via mqtt_handler.py as messages arrive from ESP32
fleet devices.
"""

from __future__ import annotations

import logging
from datetime import datetime
from typing import Generator, List, Optional

from fastapi import APIRouter, Depends, HTTPException, Query, status
from pydantic import BaseModel, ConfigDict
from sqlalchemy import desc
from sqlalchemy.exc import SQLAlchemyError
from sqlalchemy.orm import Session

from database import (
    AlertRecord,
    CrashRecord,
    Device,
    ScoreRecord,
    StatusRecord,
    TelemetryRecord,
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
    single request. sqlalchemy.orm.Session supports the context
    manager protocol directly (close()-on-exit), which is what
    get_session() returns.
    """
    session = get_session()
    try:
        yield session
    finally:
        session.close()


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

    received_at: datetime


class ScoreOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
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
    received_at: datetime

    @staticmethod
    def rating_for(score: float) -> str:
        """Simple score->label bucketing for dashboard display. Not a
        stored column since it's purely derived from `score`."""
        if score >= 90:
            return "Excellent"
        if score >= 75:
            return "Good"
        if score >= 50:
            return "Fair"
        return "Poor"


def _score_to_out(record: ScoreRecord) -> ScoreOut:
    """ScoreRecord has no `rating` column - it's purely derived from
    `score` - so it can't go through ScoreOut.model_validate(record)
    directly (that requires every required field to already be present
    on the source object). Build the dict explicitly instead."""
    return ScoreOut(
        id=record.id,
        device_id=record.device_id,
        timestamp_ms=record.timestamp_ms,
        score=record.score,
        rating=ScoreOut.rating_for(record.score),
        harsh_braking_count=record.harsh_braking_count,
        harsh_accel_count=record.harsh_accel_count,
        harsh_cornering_count=record.harsh_cornering_count,
        overspeed_count=record.overspeed_count,
        idling_seconds_total=record.idling_seconds_total,
        crash_count=record.crash_count,
        geofence_violation_count=record.geofence_violation_count,
        received_at=record.received_at,
    )


class AlertOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    device_id: str
    timestamp_ms: int
    alert_type: str
    latitude_deg: Optional[float] = None
    longitude_deg: Optional[float] = None
    raw_payload_json: str
    received_at: datetime


class CrashOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    device_id: str
    timestamp_ms: int
    total_accel_g: float
    total_gyro_dps: float
    accel_x_g: Optional[float] = None
    accel_y_g: Optional[float] = None
    accel_z_g: Optional[float] = None
    gyro_x_dps: Optional[float] = None
    gyro_y_dps: Optional[float] = None
    gyro_z_dps: Optional[float] = None
    gps_fix_valid: Optional[bool] = None
    latitude_deg: Optional[float] = None
    longitude_deg: Optional[float] = None
    speed_kmh: Optional[float] = None
    heading_deg: Optional[float] = None
    received_at: datetime


class StatusOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    device_id: str
    timestamp_ms: int
    uptime_sec: Optional[int] = None
    free_heap_bytes: Optional[int] = None
    min_free_heap_bytes: Optional[int] = None
    wifi_connected: Optional[bool] = None
    mqtt_connected: Optional[bool] = None
    wifi_rssi_dbm: Optional[int] = None
    current_driver_score: Optional[float] = None
    received_at: datetime


class DeviceSummaryOut(BaseModel):
    """Aggregated, dashboard-friendly summary for a single device."""

    device_id: str
    latest_score: Optional[float] = None
    latest_rating: Optional[str] = None
    total_alerts: int
    total_crashes: int
    last_seen_at: Optional[datetime] = None
    wifi_connected: Optional[bool] = None
    mqtt_connected: Optional[bool] = None


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------
def _run_query(db: Session, description: str, fn):
    try:
        return fn()
    except SQLAlchemyError:
        logger.exception("Database query failed: %s", description)
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail="A database error occurred while processing the request.",
        )


def _paging(limit: int, offset: int) -> tuple[int, int]:
    if offset < 0:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="offset must be >= 0.",
        )
    return min(limit, MAX_PAGE_SIZE), offset


def _require_device(db: Session, device_id: str) -> None:
    exists = db.query(Device.device_id).filter(Device.device_id == device_id).first()
    if exists is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Device {device_id} not found.",
        )


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
    one message, most recently active first."""
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
    def query():
        return db.query(Device).filter(Device.device_id == device_id).one_or_none()

    device = _run_query(db, "get_device", query)
    if device is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Device {device_id} not found.",
        )
    return device


@router.get(
    "/devices/{device_id}/summary",
    response_model=DeviceSummaryOut,
    tags=["devices"],
)
def get_device_summary(device_id: str, db: Session = Depends(get_db)) -> DeviceSummaryOut:
    """Aggregated safety summary for a device: latest score/rating,
    alert/crash counts, connectivity. Backs a single dashboard widget."""

    def query() -> DeviceSummaryOut:
        _require_device(db, device_id)

        latest_score_row = (
            db.query(ScoreRecord)
            .filter(ScoreRecord.device_id == device_id)
            .order_by(desc(ScoreRecord.timestamp_ms))
            .first()
        )
        latest_status_row = (
            db.query(StatusRecord)
            .filter(StatusRecord.device_id == device_id)
            .order_by(desc(StatusRecord.timestamp_ms))
            .first()
        )
        total_alerts = db.query(AlertRecord).filter(AlertRecord.device_id == device_id).count()
        total_crashes = db.query(CrashRecord).filter(CrashRecord.device_id == device_id).count()
        device = db.query(Device).filter(Device.device_id == device_id).one()

        return DeviceSummaryOut(
            device_id=device_id,
            latest_score=latest_score_row.score if latest_score_row else None,
            latest_rating=(
                ScoreOut.rating_for(latest_score_row.score) if latest_score_row else None
            ),
            total_alerts=total_alerts,
            total_crashes=total_crashes,
            last_seen_at=device.last_seen_at,
            wifi_connected=latest_status_row.wifi_connected if latest_status_row else None,
            mqtt_connected=latest_status_row.mqtt_connected if latest_status_row else None,
        )

    return _run_query(db, "get_device_summary", query)


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
    """Chronological (oldest-first) telemetry starting at `offset`. Use
    /telemetry/latest for a live dashboard tile instead."""
    limit, offset = _paging(limit, offset)

    def query():
        _require_device(db, device_id)
        return (
            db.query(TelemetryRecord)
            .filter(TelemetryRecord.device_id == device_id)
            .order_by(TelemetryRecord.timestamp_ms)
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
    """Most recent `limit` telemetry rows, newest first. Intended for
    polling-based live dashboard tiles."""

    def query():
        _require_device(db, device_id)
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
# --------------------------------------------------------------------------
@router.get("/devices/{device_id}/score", response_model=ScoreOut, tags=["scores"])
def get_device_latest_score(device_id: str, db: Session = Depends(get_db)) -> ScoreOut:
    """Returns the most recent driver safety score for a device, or 404
    if no score has been published yet."""

    def query():
        _require_device(db, device_id)
        return (
            db.query(ScoreRecord)
            .filter(ScoreRecord.device_id == device_id)
            .order_by(desc(ScoreRecord.timestamp_ms))
            .first()
        )

    record = _run_query(db, "get_device_latest_score", query)
    if record is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"No score has been published yet for device {device_id}.",
        )
    return _score_to_out(record)


@router.get(
    "/devices/{device_id}/score/history",
    response_model=List[ScoreOut],
    tags=["scores"],
)
def get_device_score_history(
    device_id: str,
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[ScoreOut]:
    """Historical scores, most recent first. Feeds a trend chart."""
    limit, offset = _paging(limit, offset)

    def query():
        _require_device(db, device_id)
        records = (
            db.query(ScoreRecord)
            .filter(ScoreRecord.device_id == device_id)
            .order_by(desc(ScoreRecord.timestamp_ms))
            .offset(offset)
            .limit(limit)
            .all()
        )
        return [_score_to_out(r) for r in records]

    return _run_query(db, "get_device_score_history", query)


# --------------------------------------------------------------------------
# Alerts
# --------------------------------------------------------------------------
@router.get(
    "/devices/{device_id}/alerts",
    response_model=List[AlertOut],
    tags=["alerts"],
)
def get_device_alerts(
    device_id: str,
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[AlertRecord]:
    """Alerts (excessive idling, geofence violations) for a device,
    most recent first."""
    limit, offset = _paging(limit, offset)

    def query():
        _require_device(db, device_id)
        return (
            db.query(AlertRecord)
            .filter(AlertRecord.device_id == device_id)
            .order_by(desc(AlertRecord.timestamp_ms))
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "get_device_alerts", query)


@router.get("/alerts", response_model=List[AlertOut], tags=["alerts"])
def list_alerts(
    device_id: Optional[str] = Query(None, description="Filter by device id."),
    alert_type: Optional[str] = Query(
        None, description="Filter by alert type, e.g. 'excessive_idling' or 'geofence_violation'."
    ),
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[AlertRecord]:
    """Fleet-wide alerts, most recent first, optionally filtered by
    device and/or alert type. Backs a fleet-ops "recent incidents" feed."""
    limit, offset = _paging(limit, offset)

    def query():
        q = db.query(AlertRecord)
        if device_id is not None:
            q = q.filter(AlertRecord.device_id == device_id)
        if alert_type is not None:
            q = q.filter(AlertRecord.alert_type == alert_type)
        return q.order_by(desc(AlertRecord.timestamp_ms)).offset(offset).limit(limit).all()

    return _run_query(db, "list_alerts", query)


# --------------------------------------------------------------------------
# Crashes
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
    """Confirmed crash events for a device, most recent first."""
    limit, offset = _paging(limit, offset)

    def query():
        _require_device(db, device_id)
        return (
            db.query(CrashRecord)
            .filter(CrashRecord.device_id == device_id)
            .order_by(desc(CrashRecord.timestamp_ms))
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "get_device_crashes", query)


@router.get("/crashes", response_model=List[CrashOut], tags=["crashes"])
def list_crashes(
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[CrashRecord]:
    """Fleet-wide crash events, most recent first. This is the
    highest-severity table; a fleet dashboard should surface these
    prominently."""
    limit, offset = _paging(limit, offset)

    def query():
        return (
            db.query(CrashRecord)
            .order_by(desc(CrashRecord.timestamp_ms))
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "list_crashes", query)


# --------------------------------------------------------------------------
# Status
# --------------------------------------------------------------------------
@router.get(
    "/devices/{device_id}/status/latest",
    response_model=StatusOut,
    tags=["status"],
)
def get_device_latest_status(device_id: str, db: Session = Depends(get_db)) -> StatusRecord:
    def query():
        _require_device(db, device_id)
        return (
            db.query(StatusRecord)
            .filter(StatusRecord.device_id == device_id)
            .order_by(desc(StatusRecord.timestamp_ms))
            .first()
        )

    record = _run_query(db, "get_device_latest_status", query)
    if record is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"No status has been published yet for device {device_id}.",
        )
    return record
