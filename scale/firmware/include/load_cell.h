#pragma once
#include <HX711.h>

static constexpr int MAX_CAL_POINTS = 8;

enum class WeightState {
    Uncalibrated,
    Idle,
    NewLoad,
    StableLoad,
    LoadChangedStable,
    LoadChangedUnstable,
    LoadRemoved,
};

// A calibration point captured against a known weight. `delta` is the
// ADC-counts above `tare_raw` at the moment the point was taken — that is,
// the physical slope of the load cell at `weight_g`, which is independent
// of the current tare. Storing the delta (rather than the absolute raw
// reading) means re-taring the scale does not invalidate the calibration:
// the tare just shifts the zero reference; the delta → weight mapping
// stays the same.
struct CalPoint {
    long  delta;
    float weight_g;
};

struct CalibrationData {
    long     tare_raw  = 0;
    int      numPoints = 0;
    CalPoint points[MAX_CAL_POINTS];

    bool isValid() const { return numPoints >= 1 && tare_raw != 0; }
};

enum class RoundingMode : uint8_t { Round = 0, Truncate = 1 };

struct ScaleParams {
    int          samples        = 10;
    float        stableThreshG  = 1.0f;
    int          stableCountReq = 5;
    float        loadDetectG    = 2.0f;
    int          precision      = 1;
    RoundingMode rounding       = RoundingMode::Round;
};

class LoadCell {
public:
    void begin();
    void update();

    void tare();
    long captureRaw();
    void addCalPoint(float known_weight_g, long raw);
    void clearCalPoints();
    void calibrate(float known_weight_g);

    float getWeightG() const        { return _weightG; }
    float getDisplayWeight() const;
    WeightState getState() const    { return _state; }
    long getRaw() const;
    long getLastRaw() const         { return _lastRaw; }
    bool isCalibrated() const       { return _cal.isValid(); }
    const CalibrationData& cal()    { return _cal; }

    ScaleParams& params()           { return _params; }
    void loadCalibration();
    void saveCalibration();
    void loadParams();
    void saveParams();

private:
    HX711         _hx;
    WeightState   _state    = WeightState::Uncalibrated;
    float         _weightG  = 0.0f;
    long          _lastRaw  = 0;
    float         _lastStableG = 0.0f;
    int           _stableCount = 0;
    CalibrationData _cal;
    ScaleParams     _params;

    float _rawToWeight(long raw) const;
    long  _readRawAveraged(int samples);
    void  _setState(WeightState s);
};
