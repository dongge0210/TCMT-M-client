// AccelSensor.h — BMI284 accelerometer via IOKit HID + SHM helper
#pragma once

struct AccelData {
    double x = 0.0, y = 0.0, z = 0.0;  // acceleration in g
    bool valid = false;                 // true if a sample was read
    bool hasDevice = false;             // true if accelerometer present
};

class AccelSensor {
public:
    AccelSensor();
    ~AccelSensor();
    void Refresh();
    const AccelData& GetData() const { return data_; }
    /// Install + start SMJobBless helper. Returns true if helper started.
    static bool BlessHelper();

private:
    AccelData data_;
    int shmFd_ = -1;
    void* shmPtr_ = nullptr;           // mapped shared memory (AccelShm)
    bool TryShmRead();
    void CloseShm();
};

// Shared memory layout (must match helper)
#pragma pack(push, 1)
struct AccelShm {
    uint64_t            magic;
    volatile uint64_t   updateCount;
    int32_t             x, y, z;
    double              timestamp;
};
#pragma pack(pop)

static const char* kShmName = "tcmt-accel";
static const uint64_t kShmMagic = 0x54434D544143434CULL;  // "TCMTACCL"
static const int kShmSize = 64;
