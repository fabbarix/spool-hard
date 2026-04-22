#include "load_cell.h"
#include "config.h"
#include <Preferences.h>
#include <math.h>
#include <algorithm>

// ── Piecewise-linear interpolation ───────────────────────────

float LoadCell::_rawToWeight(long raw) const {
    // Everything happens in tare-relative ADC space. Cal points are stored
    // as deltas (ADC counts above tare at capture time), so the slope is a
    // physical property of the load cell — re-taring shifts `offset` and
    // the zero of the curve but leaves the point deltas themselves alone.
    long offset = raw - _cal.tare_raw;
    int n = _cal.numPoints;
    if (n == 0) return 0.0f;

    // Single point: straight line through (0, 0) and (delta, weight).
    if (n == 1) {
        long d = _cal.points[0].delta;
        if (d == 0) return 0.0f;
        return (float)offset / (float)d * _cal.points[0].weight_g;
    }

    // Multi-point: piecewise linear. Points are sorted by weight_g
    // ascending, so deltas are also ascending for a well-behaved cell.
    if (offset <= _cal.points[0].delta) {
        long d0 = _cal.points[0].delta;
        if (d0 == 0) return 0.0f;
        return (float)offset / (float)d0 * _cal.points[0].weight_g;
    }
    if (offset >= _cal.points[n - 1].delta) {
        long d0 = _cal.points[n - 2].delta;
        long d1 = _cal.points[n - 1].delta;
        float w0 = _cal.points[n - 2].weight_g;
        float w1 = _cal.points[n - 1].weight_g;
        if (d1 == d0) return w1;
        return w0 + (float)(offset - d0) / (float)(d1 - d0) * (w1 - w0);
    }
    for (int i = 0; i < n - 1; i++) {
        long d0 = _cal.points[i].delta;
        long d1 = _cal.points[i + 1].delta;
        if (offset >= d0 && offset <= d1) {
            float w0 = _cal.points[i].weight_g;
            float w1 = _cal.points[i + 1].weight_g;
            if (d1 == d0) return w0;
            return w0 + (float)(offset - d0) / (float)(d1 - d0) * (w1 - w0);
        }
    }
    return 0.0f;
}

// ── Lifecycle ────────────────────────────────────────────────

void LoadCell::begin() {
    _hx.begin(HX711_DATA_PIN, HX711_CLK_PIN);
    loadCalibration();
    loadParams();
    _state = _cal.isValid() ? WeightState::Idle : WeightState::Uncalibrated;
    Serial.printf("[LoadCell] %d cal points, tare=%ld, %s\n",
                  _cal.numPoints, _cal.tare_raw,
                  _cal.isValid() ? "calibrated" : "uncalibrated");
}

void LoadCell::update() {
    if (!_hx.is_ready()) return;

    long raw = _readRawAveraged(_params.samples);
    _lastRaw = raw;

    if (_cal.isValid()) {
        _weightG = _rawToWeight(raw);
    } else {
        _weightG = 0.0f;
    }

    switch (_state) {
        case WeightState::Uncalibrated:
            break;

        case WeightState::Idle:
            if (_weightG > _params.loadDetectG) {
                _setState(WeightState::NewLoad);
                _stableCount = 0;
            }
            break;

        case WeightState::NewLoad:
        case WeightState::StableLoad:
        case WeightState::LoadChangedStable:
        case WeightState::LoadChangedUnstable: {
            if (_weightG < _params.loadDetectG) {
                _setState(WeightState::LoadRemoved);
                break;
            }
            bool stable = fabsf(_weightG - _lastStableG) < _params.stableThreshG;
            if (stable) {
                _stableCount++;
                if (_stableCount >= _params.stableCountReq) {
                    if (_state == WeightState::NewLoad)
                        _setState(WeightState::StableLoad);
                    else if (_state == WeightState::LoadChangedUnstable)
                        _setState(WeightState::LoadChangedStable);
                }
            } else {
                _stableCount = 0;
                _lastStableG = _weightG;
                if (_state == WeightState::StableLoad || _state == WeightState::LoadChangedStable)
                    _setState(WeightState::LoadChangedUnstable);
            }
            break;
        }

        case WeightState::LoadRemoved:
            if (_weightG < _params.loadDetectG)
                _setState(WeightState::Idle);
            else
                _setState(WeightState::NewLoad);
            break;
    }
}

// ── Calibration actions ──────────────────────────────────────

void LoadCell::tare() {
    _cal.tare_raw = _readRawAveraged(20);
    saveCalibration();
    Serial.printf("[LoadCell] Tare: raw=%ld\n", _cal.tare_raw);
}

long LoadCell::captureRaw() {
    return _readRawAveraged(20);
}

void LoadCell::addCalPoint(float known_weight_g, long raw) {
    if (_cal.numPoints >= MAX_CAL_POINTS) {
        Serial.println("[LoadCell] Max calibration points reached");
        return;
    }
    // Store as ADC delta from the current tare, not the absolute raw. This
    // is what keeps the calibration valid across future re-tares — the
    // delta is the load-cell's slope, independent of the zero reference.
    long delta = raw - _cal.tare_raw;
    _cal.points[_cal.numPoints++] = { delta, known_weight_g };

    // Sort points by weight ascending
    std::sort(_cal.points, _cal.points + _cal.numPoints,
              [](const CalPoint& a, const CalPoint& b) { return a.weight_g < b.weight_g; });

    _state = WeightState::Idle;
    saveCalibration();
    Serial.printf("[LoadCell] Added cal point: %.1fg @ delta=%ld (raw=%ld, tare=%ld; %d total)\n",
                  known_weight_g, delta, raw, _cal.tare_raw, _cal.numPoints);
}

void LoadCell::clearCalPoints() {
    _cal.numPoints = 0;
    _state = WeightState::Uncalibrated;
    saveCalibration();
    Serial.println("[LoadCell] Calibration cleared");
}

void LoadCell::calibrate(float known_weight_g) {
    // Legacy single-point calibrate (used by console protocol)
    if (_cal.tare_raw == 0) {
        tare();
    }
    long raw = captureRaw();
    clearCalPoints();
    addCalPoint(known_weight_g, raw);
}

float LoadCell::getDisplayWeight() const {
    float factor = powf(10.0f, _params.precision);
    if (_params.rounding == RoundingMode::Truncate) {
        return truncf(_weightG * factor) / factor;
    }
    return roundf(_weightG * factor) / factor;
}

long LoadCell::getRaw() const {
    return const_cast<HX711&>(_hx).read_average(5);
}

long LoadCell::_readRawAveraged(int samples) {
    long sum = 0;
    int got = 0;
    for (int i = 0; i < samples; i++) {
        unsigned long t0 = millis();
        while (!_hx.is_ready()) {
            if (millis() - t0 > 500) break;
            delay(1);
        }
        if (_hx.is_ready()) {
            sum += _hx.read();
            got++;
        }
    }
    return got > 0 ? sum / got : 0;
}

void LoadCell::_setState(WeightState s) {
    _state = s;
    _lastStableG = _weightG;
}

// ── NVS persistence ──────────────────────────────────────────

// On-disk layout version for the calibration blob:
//   v1 (implicit — legacy) : CalPoint.delta actually held the absolute raw
//                             ADC reading. Re-taring silently drifted cal.
//   v2                     : CalPoint.delta is true delta = raw - tare at
//                             capture. Invariant under re-tare.
// Loaded records with an implicit or v1 tag are migrated to v2 in place.
static constexpr int CAL_SCHEMA_VERSION = 2;

void LoadCell::loadCalibration() {
    Preferences prefs;
    prefs.begin(NVS_NS_CALIBRATION, true);
    _cal.tare_raw  = prefs.getLong(NVS_KEY_ZERO, 0);
    _cal.numPoints = prefs.getInt("num_pts", 0);
    if (_cal.numPoints > MAX_CAL_POINTS) _cal.numPoints = MAX_CAL_POINTS;
    int schema = prefs.getInt("cal_schema_v", 1);

    bool need_migration = false;

    if (_cal.numPoints > 0) {
        size_t len = sizeof(CalPoint) * _cal.numPoints;
        prefs.getBytes("cal_pts", _cal.points, len);
        if (schema < 2) need_migration = true;
    } else {
        // Legacy single-point keys from before multipoint existed. The
        // stored `legacyRaw` is an absolute reading — treat as schema v1
        // and fall through to the migration step below.
        float legacyWeight = prefs.getFloat(NVS_KEY_CALIB_WEIGHT, 0.0f);
        long  legacyRaw    = prefs.getLong(NVS_KEY_CALIB_LC, 0);
        if (legacyWeight > 0 && legacyRaw != 0 && legacyRaw != _cal.tare_raw) {
            _cal.points[0].delta    = legacyRaw;   // placeholder — migrated below
            _cal.points[0].weight_g = legacyWeight;
            _cal.numPoints = 1;
            need_migration = true;
        }
    }
    prefs.end();

    if (need_migration) {
        Serial.printf("[LoadCell] Migrating cal points v1 → v2 "
                      "(tare=%ld, %d points)\n",
                      _cal.tare_raw, _cal.numPoints);
        for (int i = 0; i < _cal.numPoints; ++i) {
            long abs_raw = _cal.points[i].delta;   // v1 field held absolute raw
            _cal.points[i].delta = abs_raw - _cal.tare_raw;
            Serial.printf("  [%d] %.1fg: raw=%ld → delta=%ld\n",
                          i, _cal.points[i].weight_g, abs_raw, _cal.points[i].delta);
        }
        saveCalibration();   // writes schema_v = 2
    }
}

void LoadCell::saveCalibration() {
    Preferences prefs;
    prefs.begin(NVS_NS_CALIBRATION, false);
    prefs.putLong(NVS_KEY_ZERO, _cal.tare_raw);
    prefs.putInt("num_pts", _cal.numPoints);
    if (_cal.numPoints > 0) {
        prefs.putBytes("cal_pts", _cal.points, sizeof(CalPoint) * _cal.numPoints);
    }
    prefs.putInt("cal_schema_v", CAL_SCHEMA_VERSION);
    prefs.end();
}

void LoadCell::loadParams() {
    Preferences prefs;
    prefs.begin(NVS_NS_SCALE, true);
    _params.samples        = prefs.getInt(NVS_KEY_SAMPLES,    WEIGHT_SAMPLES);
    _params.stableThreshG  = prefs.getFloat(NVS_KEY_STABLE_THR, STABLE_THRESHOLD_G);
    _params.stableCountReq = prefs.getInt(NVS_KEY_STABLE_CNT, STABLE_COUNT_REQ);
    _params.loadDetectG    = prefs.getFloat(NVS_KEY_LOAD_DET,   LOAD_DETECT_G);
    _params.precision      = prefs.getInt(NVS_KEY_PRECISION,  1);
    _params.rounding       = (RoundingMode)prefs.getUChar(NVS_KEY_ROUNDING, 0);
    prefs.end();
}

void LoadCell::saveParams() {
    Preferences prefs;
    prefs.begin(NVS_NS_SCALE, false);
    prefs.putInt(NVS_KEY_SAMPLES,     _params.samples);
    prefs.putFloat(NVS_KEY_STABLE_THR, _params.stableThreshG);
    prefs.putInt(NVS_KEY_STABLE_CNT,  _params.stableCountReq);
    prefs.putFloat(NVS_KEY_LOAD_DET,   _params.loadDetectG);
    prefs.putInt(NVS_KEY_PRECISION,   _params.precision);
    prefs.putUChar(NVS_KEY_ROUNDING,  (uint8_t)_params.rounding);
    prefs.end();
}
