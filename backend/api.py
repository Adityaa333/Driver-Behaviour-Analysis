"""
api.py

REST API router for the Driver Behaviour Analysis System backend.

Exposes read-only endpoints consumed primarily by the Node-RED
dashboard (and any other downstream consumer, e.g. a fleet-ops portal)
to query:
    - Drivers
    - Trips
    - Raw sensor telemetry for a trip
    - Computed driver safety scores (per-trip and historical)
    - Safety alerts (harsh braking, crashes, geofence breaches, etc.)

All write paths into the underlying tables are intentionally excluded
from this router. Telemetry and alerts are written exclusively by the
MQTT ingestion handler (mqtt_handler.py) as data arrives from ESP32
fleet devices, and score records are written exclusively by the ML
inference job (ml/inference.py). Keeping a single writer per table
avoids race conditions and keeps this API's contract purely
query-oriented, which is deliberate for a system whose output
(the safety score) must be auditable and reproducible.

See the accompanying assumptions note for the exact ORM schema this
module depends on (Driver, Trip, SensorReading, ScoreRecord, Alert),
all imported from database.py.
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
    Alert,
    Driver,
    ScoreRecord,
    SensorReading,
    Trip,
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
    single request. The underlying context manager guarantees the
    session is closed (and any uncommitted transaction rolled back)
    once the request finishes, even if an exception is raised.
    """
    with get_session() as session:
        yield session


# --------------------------------------------------------------------------
# Response schemas
# --------------------------------------------------------------------------
class DriverOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    name: str
    license_number: str
    phone: Optional[str] = None
    created_at: datetime


class TripOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    driver_id: int
    vehicle_id: int
    start_time: datetime
    end_time: Optional[datetime] = None
    start_lat: Optional[float] = None
    start_lon: Optional[float] = None
    end_lat: Optional[float] = None
    end_lon: Optional[float] = None
    distance_km: Optional[float] = None
    status: str


class SensorReadingOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    trip_id: int
    timestamp: datetime
    ax: float
    ay: float
    az: float
    gx: float
    gy: float
    gz: float
    speed_kmh: Optional[float] = None
    latitude: Optional[float] = None
    longitude: Optional[float] = None
    obd_rpm: Optional[int] = None
    obd_throttle_pct: Optional[float] = None
    obd_engine_load_pct: Optional[float] = None


class ScoreRecordOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    trip_id: int
    driver_id: int
    score: float
    harsh_braking_count: int
    harsh_accel_count: int
    sharp_turn_count: int
    overspeed_count: int
    idling_seconds: int
    computed_at: datetime


class AlertOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: int
    trip_id: int
    driver_id: int
    alert_type: str
    severity: str
    message: str
    latitude: Optional[float] = None
    longitude: Optional[float] = None
    created_at: datetime


class DriverSummaryOut(BaseModel):
    """
    Aggregated, dashboard-friendly summary for a single driver.
    Intended to back a Node-RED "driver overview" card without the
    dashboard needing to fetch and reduce raw trip/score lists itself.
    """

    driver_id: int
    total_trips: int
    average_score: Optional[float] = None
    latest_score: Optional[float] = None
    total_alerts: int
    critical_alerts: int


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------
def _run_query(db: Session, description: str, fn):
    """
    Executes a query callable, translating SQLAlchemy failures into a
    uniform 500 response while logging full detail server-side. This
    keeps error handling consistent across every endpoint below
    instead of repeating identical try/except blocks.
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
    negative offsets rather than silently coercing them, since a
    negative offset most likely indicates a client-side bug worth
    surfacing rather than masking.
    """
    if offset < 0:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="offset must be >= 0.",
        )
    return min(limit, MAX_PAGE_SIZE), offset


# --------------------------------------------------------------------------
# Drivers
# --------------------------------------------------------------------------
@router.get("/drivers", response_model=List[DriverOut], tags=["drivers"])
def list_drivers(
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[Driver]:
    """Returns a paginated list of registered drivers, ordered by id."""
    limit, offset = _paging(limit, offset)

    def query():
        return (
            db.query(Driver)
            .order_by(Driver.id)
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "list_drivers", query)


@router.get("/drivers/{driver_id}", response_model=DriverOut, tags=["drivers"])
def get_driver(driver_id: int, db: Session = Depends(get_db)) -> Driver:
    """Returns a single driver by id, or 404 if no such driver exists."""

    def query():
        return db.query(Driver).filter(Driver.id == driver_id).one_or_none()

    driver = _run_query(db, "get_driver", query)
    if driver is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Driver {driver_id} not found.",
        )
    return driver


@router.get(
    "/drivers/{driver_id}/summary",
    response_model=DriverSummaryOut,
    tags=["drivers"],
)
def get_driver_summary(driver_id: int, db: Session = Depends(get_db)) -> DriverSummaryOut:
    """
    Returns an aggregated safety summary for a driver: trip count,
    average and latest safety score, and alert counts by severity.
    Designed to back a single Node-RED dashboard widget in one call.
    """

    def query() -> DriverSummaryOut:
        driver_exists = db.query(Driver.id).filter(Driver.id == driver_id).first()
        if driver_exists is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Driver {driver_id} not found.",
            )

        total_trips = (
            db.query(Trip).filter(Trip.driver_id == driver_id).count()
        )

        scores = (
            db.query(ScoreRecord.score)
            .filter(ScoreRecord.driver_id == driver_id)
            .all()
        )
        score_values = [s[0] for s in scores]
        average_score = (
            sum(score_values) / len(score_values) if score_values else None
        )

        latest_score_row = (
            db.query(ScoreRecord)
            .filter(ScoreRecord.driver_id == driver_id)
            .order_by(desc(ScoreRecord.computed_at))
            .first()
        )
        latest_score = latest_score_row.score if latest_score_row else None

        total_alerts = (
            db.query(Alert).filter(Alert.driver_id == driver_id).count()
        )
        critical_alerts = (
            db.query(Alert)
            .filter(Alert.driver_id == driver_id, Alert.severity == "critical")
            .count()
        )

        return DriverSummaryOut(
            driver_id=driver_id,
            total_trips=total_trips,
            average_score=average_score,
            latest_score=latest_score,
            total_alerts=total_alerts,
            critical_alerts=critical_alerts,
        )

    return _run_query(db, "get_driver_summary", query)


@router.get("/drivers/{driver_id}/trips", response_model=List[TripOut], tags=["trips"])
def list_driver_trips(
    driver_id: int,
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[Trip]:
    """Returns a paginated list of trips for a given driver, most recent first."""
    limit, offset = _paging(limit, offset)

    def query():
        driver_exists = db.query(Driver.id).filter(Driver.id == driver_id).first()
        if driver_exists is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Driver {driver_id} not found.",
            )
        return (
            db.query(Trip)
            .filter(Trip.driver_id == driver_id)
            .order_by(desc(Trip.start_time))
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "list_driver_trips", query)


@router.get(
    "/drivers/{driver_id}/score/history",
    response_model=List[ScoreRecordOut],
    tags=["scores"],
)
def get_driver_score_history(
    driver_id: int,
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[ScoreRecord]:
    """
    Returns a driver's historical safety scores, most recent first.
    Intended to feed a trend chart on the Node-RED dashboard.
    """
    limit, offset = _paging(limit, offset)

    def query():
        driver_exists = db.query(Driver.id).filter(Driver.id == driver_id).first()
        if driver_exists is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Driver {driver_id} not found.",
            )
        return (
            db.query(ScoreRecord)
            .filter(ScoreRecord.driver_id == driver_id)
            .order_by(desc(ScoreRecord.computed_at))
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "get_driver_score_history", query)


# --------------------------------------------------------------------------
# Trips
# --------------------------------------------------------------------------
@router.get("/trips/{trip_id}", response_model=TripOut, tags=["trips"])
def get_trip(trip_id: int, db: Session = Depends(get_db)) -> Trip:
    """Returns a single trip by id, or 404 if no such trip exists."""

    def query():
        return db.query(Trip).filter(Trip.id == trip_id).one_or_none()

    trip = _run_query(db, "get_trip", query)
    if trip is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Trip {trip_id} not found.",
        )
    return trip


@router.get(
    "/trips/{trip_id}/telemetry",
    response_model=List[SensorReadingOut],
    tags=["telemetry"],
)
def get_trip_telemetry(
    trip_id: int,
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[SensorReading]:
    """
    Returns raw sensor telemetry for a trip, ordered chronologically
    (oldest first) starting at `offset`. Use `/telemetry/latest` for
    the most recent readings instead, e.g. for a "live" dashboard tile.
    """
    limit, offset = _paging(limit, offset)

    def query():
        trip_exists = db.query(Trip.id).filter(Trip.id == trip_id).first()
        if trip_exists is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Trip {trip_id} not found.",
            )
        return (
            db.query(SensorReading)
            .filter(SensorReading.trip_id == trip_id)
            .order_by(SensorReading.timestamp)
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "get_trip_telemetry", query)


@router.get(
    "/trips/{trip_id}/telemetry/latest",
    response_model=List[SensorReadingOut],
    tags=["telemetry"],
)
def get_trip_latest_telemetry(
    trip_id: int,
    limit: int = Query(20, ge=1, le=MAX_PAGE_SIZE),
    db: Session = Depends(get_db),
) -> List[SensorReading]:
    """
    Returns the most recent `limit` sensor readings for a trip, newest
    first. Intended for near-real-time dashboard tiles that poll this
    endpoint rather than replaying an entire trip's telemetry.
    """

    def query():
        trip_exists = db.query(Trip.id).filter(Trip.id == trip_id).first()
        if trip_exists is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Trip {trip_id} not found.",
            )
        return (
            db.query(SensorReading)
            .filter(SensorReading.trip_id == trip_id)
            .order_by(desc(SensorReading.timestamp))
            .limit(limit)
            .all()
        )

    return _run_query(db, "get_trip_latest_telemetry", query)


@router.get("/trips/{trip_id}/score", response_model=ScoreRecordOut, tags=["scores"])
def get_trip_score(trip_id: int, db: Session = Depends(get_db)) -> ScoreRecord:
    """
    Returns the computed safety score for a trip, or 404 if scoring
    has not yet run for this trip (e.g. the trip is still in progress
    or the ML inference job has not processed it yet).
    """

    def query():
        trip_exists = db.query(Trip.id).filter(Trip.id == trip_id).first()
        if trip_exists is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Trip {trip_id} not found.",
            )
        return (
            db.query(ScoreRecord)
            .filter(ScoreRecord.trip_id == trip_id)
            .one_or_none()
        )

    score = _run_query(db, "get_trip_score", query)
    if score is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"No score has been computed yet for trip {trip_id}.",
        )
    return score


@router.get("/trips/{trip_id}/alerts", response_model=List[AlertOut], tags=["alerts"])
def get_trip_alerts(
    trip_id: int,
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[Alert]:
    """Returns alerts raised during a specific trip, most recent first."""
    limit, offset = _paging(limit, offset)

    def query():
        trip_exists = db.query(Trip.id).filter(Trip.id == trip_id).first()
        if trip_exists is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Trip {trip_id} not found.",
            )
        return (
            db.query(Alert)
            .filter(Alert.trip_id == trip_id)
            .order_by(desc(Alert.created_at))
            .offset(offset)
            .limit(limit)
            .all()
        )

    return _run_query(db, "get_trip_alerts", query)


# --------------------------------------------------------------------------
# Alerts (fleet-wide)
# --------------------------------------------------------------------------
@router.get("/alerts", response_model=List[AlertOut], tags=["alerts"])
def list_alerts(
    driver_id: Optional[int] = Query(None, description="Filter by driver id."),
    severity: Optional[str] = Query(
        None, description="Filter by severity: info, warning, or critical."
    ),
    limit: int = Query(DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    offset: int = Query(0, ge=0),
    db: Session = Depends(get_db),
) -> List[Alert]:
    """
    Returns fleet-wide alerts, most recent first, optionally filtered
    by driver and/or severity. Intended to back a fleet-ops "recent
    incidents" feed on the Node-RED dashboard.
    """
    limit, offset = _paging(limit, offset)

    valid_severities = {"info", "warning", "critical"}
    if severity is not None and severity not in valid_severities:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=f"severity must be one of {sorted(valid_severities)}.",
        )

    def query():
        q = db.query(Alert)
        if driver_id is not None:
            q = q.filter(Alert.driver_id == driver_id)
        if severity is not None:
            q = q.filter(Alert.severity == severity)
        return q.order_by(desc(Alert.created_at)).offset(offset).limit(limit).all()

    return _run_query(db, "list_alerts", query)
