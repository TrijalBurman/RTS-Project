#include <WiFi.h>
#include <WebServer.h>

// Set to 0 to intentionally expose race conditions for comparison.
#define USE_MUTEX_PROTECTION 1

// Set to 1 to serialize access requests through a binary semaphore gate.
#define USE_ACCESS_SEMAPHORE 1

static constexpr uint8_t HIGH_TASK_LED_PIN = 25;
static constexpr uint8_t MEDIUM_TASK_LED_PIN = 26;
static constexpr uint8_t LOW_TASK_LED_PIN = 27;
static constexpr uint8_t RESOURCE_LED_PIN = 33;

static constexpr uint32_t SERIAL_BAUD_RATE = 115200;
static constexpr uint16_t TASK_STACK_SIZE = 4096;

static constexpr char DASHBOARD_AP_SSID[] = "ESP32-RTOS-Dashboard";
static constexpr char DASHBOARD_AP_PASSWORD[] = "esp32rtos";
static constexpr uint16_t DASHBOARD_PORT = 80;

static constexpr uint16_t WORK_MIN_MS = 80;
static constexpr uint16_t WORK_MAX_MS = 220;
static constexpr uint16_t IDLE_MIN_MS = 60;
static constexpr uint16_t IDLE_MAX_MS = 180;
static constexpr uint16_t RESOURCE_HOLD_MIN_MS = 40;
static constexpr uint16_t RESOURCE_HOLD_MAX_MS = 120;

static constexpr uint32_t METRICS_PERIOD_MS = 2000;

typedef struct
{
    const char *name;
    UBaseType_t priority;
    uint8_t ledPin;
    uint16_t blinkMs;
} TaskConfig;

typedef struct
{
    volatile uint32_t attempts;
    volatile uint32_t successes;
    volatile uint32_t blocked;
    volatile uint32_t lastObservedCounter;
} TaskStats;

static volatile int32_t sharedCounter = 0;
static volatile bool resourceActive = false;
static SemaphoreHandle_t resourceMutex = nullptr;
static SemaphoreHandle_t accessSemaphore = nullptr;
static portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
static WebServer dashboardServer(DASHBOARD_PORT);
static const char *lastResourceOwner = "NONE";
static bool systemReady = false;

static TaskStats highStats = {0, 0, 0, 0};
static TaskStats mediumStats = {0, 0, 0, 0};
static TaskStats lowStats = {0, 0, 0, 0};

static const TaskConfig HIGH_TASK = {"HIGH", 3, HIGH_TASK_LED_PIN, 80};
static const TaskConfig MEDIUM_TASK = {"MEDIUM", 2, MEDIUM_TASK_LED_PIN, 130};
static const TaskConfig LOW_TASK = {"LOW", 1, LOW_TASK_LED_PIN, 180};

static int randomRangeMs(int minMs, int maxMs)
{
    return random(minMs, maxMs + 1);
}

static void captureStatsSnapshot(
    TaskStats &highCopy,
    TaskStats &mediumCopy,
    TaskStats &lowCopy,
    int32_t &counterCopy,
    bool &resourceActiveCopy,
    const char *&ownerCopy)
{
    portENTER_CRITICAL(&statsMux);
    highCopy = highStats;
    mediumCopy = mediumStats;
    lowCopy = lowStats;
    counterCopy = sharedCounter;
    resourceActiveCopy = resourceActive;
    ownerCopy = lastResourceOwner;
    portEXIT_CRITICAL(&statsMux);
}

static void pulseLed(uint8_t pin, uint16_t highMs)
{
    digitalWrite(pin, HIGH);
    vTaskDelay(pdMS_TO_TICKS(highMs));
    digitalWrite(pin, LOW);
}

static void updateStat(volatile uint32_t &field, uint32_t value)
{
    portENTER_CRITICAL(&statsMux);
    field += value;
    portEXIT_CRITICAL(&statsMux);
}

static void setLastCounter(TaskStats *stats, int32_t value)
{
    portENTER_CRITICAL(&statsMux);
    stats->lastObservedCounter = static_cast<uint32_t>(value);
    portEXIT_CRITICAL(&statsMux);
}

static void setResourceState(bool active, const char *owner)
{
    portENTER_CRITICAL(&statsMux);
    resourceActive = active;
    if (owner != nullptr)
    {
        lastResourceOwner = owner;
    }
    portEXIT_CRITICAL(&statsMux);
}

static bool tryEnterResource(const TaskConfig *config, TaskStats *stats)
{
    bool semaphoreGranted = true;

#if USE_ACCESS_SEMAPHORE
    if (accessSemaphore == nullptr)
    {
        return false;
    }

    semaphoreGranted = (xSemaphoreTake(accessSemaphore, pdMS_TO_TICKS(40)) == pdTRUE);
    if (!semaphoreGranted)
    {
        updateStat(stats->blocked, 1);
        Serial.printf("[%s] Access semaphore blocked\r\n", config->name);
        return false;
    }
#endif

#if USE_MUTEX_PROTECTION
    if (resourceMutex == nullptr)
    {
#if USE_ACCESS_SEMAPHORE
        xSemaphoreGive(accessSemaphore);
#endif
        return false;
    }

    if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(80)) != pdTRUE)
    {
        updateStat(stats->blocked, 1);
        Serial.printf("[%s] Mutex timeout\r\n", config->name);
#if USE_ACCESS_SEMAPHORE
        xSemaphoreGive(accessSemaphore);
#endif
        return false;
    }
#endif

    return true;
}

static void leaveResource()
{
#if USE_MUTEX_PROTECTION
    xSemaphoreGive(resourceMutex);
#endif

#if USE_ACCESS_SEMAPHORE
    xSemaphoreGive(accessSemaphore);
#endif
}

static void accessSharedResource(const TaskConfig *config, TaskStats *stats)
{
    updateStat(stats->attempts, 1);

    if (!tryEnterResource(config, stats))
    {
        return;
    }

    setResourceState(true, config->name);
    digitalWrite(RESOURCE_LED_PIN, HIGH);

    const int32_t snapshot = sharedCounter;
    vTaskDelay(pdMS_TO_TICKS(randomRangeMs(RESOURCE_HOLD_MIN_MS, RESOURCE_HOLD_MAX_MS)));
    sharedCounter = snapshot + 1;

    digitalWrite(RESOURCE_LED_PIN, LOW);
    setResourceState(false, config->name);
    leaveResource();

    updateStat(stats->successes, 1);
    setLastCounter(stats, sharedCounter);

    Serial.printf(
        "[%s] priority=%u counter=%ld attempts=%lu successes=%lu blocked=%lu\r\n",
        config->name,
        static_cast<unsigned>(config->priority),
        static_cast<long>(sharedCounter),
        static_cast<unsigned long>(stats->attempts),
        static_cast<unsigned long>(stats->successes),
        static_cast<unsigned long>(stats->blocked));
}

static void stressTask(void *parameter)
{
    const TaskConfig *config = static_cast<const TaskConfig *>(parameter);
    TaskStats *stats = nullptr;

    if (config == &HIGH_TASK)
    {
        stats = &highStats;
    }
    else if (config == &MEDIUM_TASK)
    {
        stats = &mediumStats;
    }
    else
    {
        stats = &lowStats;
    }

    pinMode(config->ledPin, OUTPUT);
    digitalWrite(config->ledPin, LOW);

    for (;;)
    {
        pulseLed(config->ledPin, config->blinkMs);
        vTaskDelay(pdMS_TO_TICKS(randomRangeMs(WORK_MIN_MS, WORK_MAX_MS)));
        accessSharedResource(config, stats);
        vTaskDelay(pdMS_TO_TICKS(randomRangeMs(IDLE_MIN_MS, IDLE_MAX_MS)));
    }
}

static void metricsTask(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        TaskStats highCopy;
        TaskStats mediumCopy;
        TaskStats lowCopy;
        int32_t counterCopy = 0;
        bool resourceActiveCopy = false;
        const char *ownerCopy = "NONE";

        captureStatsSnapshot(highCopy, mediumCopy, lowCopy, counterCopy, resourceActiveCopy, ownerCopy);

        Serial.println();
        Serial.println("===== RTOS Shared Resource Metrics =====");
        Serial.printf("Global Counter: %ld\r\n", static_cast<long>(counterCopy));
        Serial.printf(
            "HIGH   -> attempts=%lu successes=%lu blocked=%lu last=%lu\r\n",
            static_cast<unsigned long>(highCopy.attempts),
            static_cast<unsigned long>(highCopy.successes),
            static_cast<unsigned long>(highCopy.blocked),
            static_cast<unsigned long>(highCopy.lastObservedCounter));
        Serial.printf(
            "MEDIUM -> attempts=%lu successes=%lu blocked=%lu last=%lu\r\n",
            static_cast<unsigned long>(mediumCopy.attempts),
            static_cast<unsigned long>(mediumCopy.successes),
            static_cast<unsigned long>(mediumCopy.blocked),
            static_cast<unsigned long>(mediumCopy.lastObservedCounter));
        Serial.printf(
            "LOW    -> attempts=%lu successes=%lu blocked=%lu last=%lu\r\n",
            static_cast<unsigned long>(lowCopy.attempts),
            static_cast<unsigned long>(lowCopy.successes),
            static_cast<unsigned long>(lowCopy.blocked),
            static_cast<unsigned long>(lowCopy.lastObservedCounter));
        Serial.printf("RESOURCE -> active=%s owner=%s\r\n", resourceActiveCopy ? "YES" : "NO", ownerCopy);
        Serial.printf(
            "Protection: mutex=%s, accessSemaphore=%s\r\n",
#if USE_MUTEX_PROTECTION
            "ON",
#else
            "OFF",
#endif
#if USE_ACCESS_SEMAPHORE
            "ON"
#else
            "OFF"
#endif
        );
        Serial.println("========================================");
        Serial.println();

        vTaskDelay(pdMS_TO_TICKS(METRICS_PERIOD_MS));
    }
}

static void handleDashboardRoot()
{
    static const char html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 RTOS Dashboard</title>
  <style>
    :root {
      --bg: #08111f;
      --bg2: #132746;
      --panel: rgba(17, 28, 48, 0.88);
      --line: rgba(255, 255, 255, 0.09);
      --text: #edf4ff;
      --muted: #97aac8;
      --accent: #3ae6d0;
      --high: #ff8a5b;
      --medium: #ffd166;
      --low: #79c7ff;
      --danger: #ff6d8d;
      --good: #51e59b;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: "Segoe UI", Tahoma, sans-serif;
      background:
        radial-gradient(circle at 15% 15%, rgba(58, 230, 208, 0.16), transparent 25%),
        radial-gradient(circle at 85% 10%, rgba(121, 199, 255, 0.14), transparent 22%),
        linear-gradient(160deg, var(--bg2) 0%, var(--bg) 58%);
      color: var(--text);
      min-height: 100vh;
    }
    .wrap {
      max-width: 1200px;
      margin: 0 auto;
      padding: 24px 20px 40px;
    }
    .hero, .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 22px;
      box-shadow: 0 20px 60px rgba(0, 0, 0, 0.28);
      backdrop-filter: blur(10px);
    }
    .hero {
      padding: 28px;
      position: relative;
      overflow: hidden;
      margin-bottom: 18px;
    }
    .hero::after {
      content: "";
      position: absolute;
      inset: auto -10% -40% auto;
      width: 280px;
      height: 280px;
      border-radius: 50%;
      background: radial-gradient(circle, rgba(58, 230, 208, 0.14), transparent 65%);
    }
    .eyebrow {
      display: inline-flex;
      align-items: center;
      gap: 10px;
      color: var(--accent);
      font-size: 0.82rem;
      text-transform: uppercase;
      letter-spacing: 0.18em;
      margin-bottom: 14px;
    }
    .dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: var(--accent);
      box-shadow: 0 0 14px rgba(58, 230, 208, 0.7);
    }
    h1 {
      margin: 0;
      max-width: 760px;
      font-size: clamp(2rem, 4.8vw, 4rem);
      line-height: 1.05;
    }
    .hero-copy {
      margin-top: 12px;
      max-width: 780px;
      color: var(--muted);
      font-size: 1.05rem;
      line-height: 1.55;
    }
    .hero-grid,
    .summary-grid,
    .task-grid,
    .meta-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 16px;
    }
    .hero-grid {
      margin-top: 24px;
    }
    .summary-grid {
      margin-bottom: 18px;
    }
    .meta-grid {
      grid-template-columns: 1.2fr 0.8fr;
      margin-bottom: 18px;
    }
    .panel {
      padding: 18px;
    }
    .metric-card {
      min-height: 132px;
      display: flex;
      flex-direction: column;
      justify-content: space-between;
    }
    .label,
    .section-label {
      color: var(--muted);
      font-size: 0.82rem;
      text-transform: uppercase;
      letter-spacing: 0.14em;
    }
    .value {
      font-size: clamp(1.9rem, 4vw, 3rem);
      font-weight: 700;
      margin-top: 10px;
    }
    .mini {
      margin-top: 8px;
      color: var(--muted);
      font-size: 0.95rem;
    }
    .task-card {
      padding: 20px;
    }
    .task-title {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      margin-bottom: 14px;
    }
    .task-name {
      font-weight: 700;
      font-size: 1.1rem;
    }
    .task-sub {
      color: var(--muted);
      font-size: 0.9rem;
      margin-top: 4px;
    }
    .badge {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: 6px 12px;
      border-radius: 999px;
      font-size: 0.8rem;
      font-weight: 700;
      letter-spacing: 0.04em;
    }
    .badge-high {
      background: rgba(255, 138, 91, 0.14);
      color: var(--high);
    }
    .badge-medium {
      background: rgba(255, 209, 102, 0.14);
      color: var(--medium);
    }
    .badge-low {
      background: rgba(121, 199, 255, 0.14);
      color: var(--low);
    }
    .status-on { color: var(--good); }
    .status-off { color: var(--danger); }
    .stats-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 12px;
      margin-top: 16px;
    }
    .stat-box {
      padding: 12px 14px;
      background: rgba(255, 255, 255, 0.025);
      border: 1px solid rgba(255, 255, 255, 0.05);
      border-radius: 14px;
    }
    .stat-box .stat-label {
      color: var(--muted);
      font-size: 0.82rem;
    }
    .stat-box .stat-value {
      font-size: 1.35rem;
      font-weight: 700;
      margin-top: 6px;
    }
    .bar-wrap {
      margin-top: 16px;
    }
    .bar-head {
      display: flex;
      justify-content: space-between;
      color: var(--muted);
      font-size: 0.9rem;
      margin-bottom: 8px;
    }
    .bar {
      height: 12px;
      border-radius: 999px;
      background: rgba(255, 255, 255, 0.06);
      overflow: hidden;
    }
    .bar > span {
      display: block;
      height: 100%;
      border-radius: inherit;
      width: 0%;
      transition: width 0.35s ease;
    }
    .bar.success > span {
      background: linear-gradient(90deg, rgba(81,229,155,0.65), rgba(58,230,208,1));
    }
    .bar.blocked > span {
      background: linear-gradient(90deg, rgba(255,109,141,0.55), rgba(255,109,141,0.95));
    }
    .history {
      display: grid;
      grid-template-columns: repeat(16, 1fr);
      gap: 6px;
      height: 54px;
      align-items: end;
      margin-top: 16px;
    }
    .history span {
      display: block;
      border-radius: 999px 999px 6px 6px;
      min-height: 8px;
      background: rgba(255, 255, 255, 0.08);
      transition: height 0.35s ease, background 0.35s ease;
    }
    .activity-legend {
      display: flex;
      justify-content: space-between;
      margin-top: 8px;
      color: var(--muted);
      font-size: 0.8rem;
    }
    .abstract {
      color: var(--muted);
      line-height: 1.65;
      font-size: 0.97rem;
    }
    .list {
      display: grid;
      gap: 12px;
    }
    .list-row {
      display: flex;
      justify-content: space-between;
      gap: 12px;
      padding-bottom: 10px;
      border-bottom: 1px solid rgba(255, 255, 255, 0.06);
    }
    .list-row span {
      color: var(--muted);
    }
    .list-row strong {
      color: var(--text);
      text-align: right;
    }
    .refresh {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      margin-top: 18px;
      color: var(--muted);
      font-size: 0.9rem;
    }
    .tick {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: var(--accent);
      box-shadow: 0 0 12px rgba(58, 230, 208, 0.6);
    }
    @media (max-width: 900px) {
      .meta-grid {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <section class="hero">
      <div class="eyebrow"><span class="dot"></span> Local RTOS Observer</div>
      <h1>ESP32 Shared Resource Stress Dashboard</h1>
      <div class="hero-copy">
        Real-time visualization of FreeRTOS scheduling, shared-resource contention, and synchronization behavior on ESP32 using dedicated LEDs and a local browser dashboard.
      </div>
      <div class="hero-grid">
        <div class="panel metric-card">
          <div class="label">Global Counter</div>
          <div class="value" id="counter">0</div>
          <div class="mini">Successful accesses to the shared resource</div>
        </div>
        <div class="panel metric-card">
          <div class="label">Mutex Protection</div>
          <div class="value" id="mutexState">ON</div>
          <div class="mini">Prevents unsafe simultaneous writes</div>
        </div>
        <div class="panel metric-card">
          <div class="label">Access Semaphore</div>
          <div class="value" id="semaphoreState">ON</div>
          <div class="mini">Shows blocking behavior during contention</div>
        </div>
        <div class="panel metric-card">
          <div class="label">Overall Success Rate</div>
          <div class="value" id="overallSuccessRate">0%</div>
          <div class="mini">Derived from total successes vs attempts</div>
        </div>
      </div>
    </section>

    <section class="summary-grid">
      <div class="panel">
        <div class="section-label">System Snapshot</div>
        <div class="stats-grid">
          <div class="stat-box"><div class="stat-label">Total Attempts</div><div class="stat-value" id="totalAttempts">0</div></div>
          <div class="stat-box"><div class="stat-label">Total Blocked</div><div class="stat-value" id="totalBlocked">0</div></div>
          <div class="stat-box"><div class="stat-label">Contention Level</div><div class="stat-value" id="contentionLevel">Low</div></div>
          <div class="stat-box"><div class="stat-label">Most Active Task</div><div class="stat-value" id="mostActiveTask">High</div></div>
        </div>
      </div>
      <div class="panel">
        <div class="section-label">LED Mapping</div>
        <div class="list" style="margin-top:14px;">
          <div class="list-row"><span>High Priority LED</span><strong>GPIO 25</strong></div>
          <div class="list-row"><span>Medium Priority LED</span><strong>GPIO 26</strong></div>
          <div class="list-row"><span>Low Priority LED</span><strong>GPIO 27</strong></div>
          <div class="list-row"><span>Shared Resource LED</span><strong>GPIO 33</strong></div>
        </div>
      </div>
    </section>

    <section class="meta-grid">
      <div class="panel">
        <div class="section-label">Project Abstract</div>
        <p class="abstract">
          This local dashboard demonstrates how multiple FreeRTOS tasks with different priorities compete for a shared resource on ESP32. By combining synchronization primitives with LED-based visualization and live browser metrics, the system highlights contention, blocking, safe access control, and comparative multitasking behavior under stress.
        </p>
      </div>
      <div class="panel">
        <div class="section-label">Run Status</div>
        <div class="list" style="margin-top:14px;">
          <div class="list-row"><span>Dashboard Refresh</span><strong>Every 1 second</strong></div>
          <div class="list-row"><span>Execution Mode</span><strong>FreeRTOS Multi-tasking</strong></div>
          <div class="list-row"><span>Protection Status</span><strong id="protectionSummary">Dual sync active</strong></div>
          <div class="list-row"><span>Task Health</span><strong id="healthSummary">Stable</strong></div>
        </div>
      </div>
    </section>

    <section class="task-grid">
      <div class="panel task-card">
        <div class="task-title">
          <div>
            <div class="task-name">High Priority Task</div>
            <div class="task-sub">Fastest scheduled worker</div>
          </div>
          <span class="badge badge-high">P3</span>
        </div>
        <div class="stats-grid">
          <div class="stat-box"><div class="stat-label">Attempts</div><div class="stat-value" id="highAttempts">0</div></div>
          <div class="stat-box"><div class="stat-label">Successes</div><div class="stat-value" id="highSuccesses">0</div></div>
          <div class="stat-box"><div class="stat-label">Blocked</div><div class="stat-value" id="highBlocked">0</div></div>
          <div class="stat-box"><div class="stat-label">Last Counter</div><div class="stat-value" id="highLast">0</div></div>
        </div>
        <div class="bar-wrap">
          <div class="bar-head"><span>Success Efficiency</span><strong id="highRate">0%</strong></div>
          <div class="bar success"><span id="highRateBar"></span></div>
        </div>
        <div class="bar-wrap">
          <div class="bar-head"><span>Blocking Pressure</span><strong id="highBlockedRate">0%</strong></div>
          <div class="bar blocked"><span id="highBlockedBar"></span></div>
        </div>
        <div class="history" id="highHistory"></div>
        <div class="activity-legend"><span>Recent activity trend</span><span id="highTrend">Stable</span></div>
      </div>
      <div class="panel task-card">
        <div class="task-title">
          <div>
            <div class="task-name">Medium Priority Task</div>
            <div class="task-sub">Balanced worker under contention</div>
          </div>
          <span class="badge badge-medium">P2</span>
        </div>
        <div class="stats-grid">
          <div class="stat-box"><div class="stat-label">Attempts</div><div class="stat-value" id="mediumAttempts">0</div></div>
          <div class="stat-box"><div class="stat-label">Successes</div><div class="stat-value" id="mediumSuccesses">0</div></div>
          <div class="stat-box"><div class="stat-label">Blocked</div><div class="stat-value" id="mediumBlocked">0</div></div>
          <div class="stat-box"><div class="stat-label">Last Counter</div><div class="stat-value" id="mediumLast">0</div></div>
        </div>
        <div class="bar-wrap">
          <div class="bar-head"><span>Success Efficiency</span><strong id="mediumRate">0%</strong></div>
          <div class="bar success"><span id="mediumRateBar"></span></div>
        </div>
        <div class="bar-wrap">
          <div class="bar-head"><span>Blocking Pressure</span><strong id="mediumBlockedRate">0%</strong></div>
          <div class="bar blocked"><span id="mediumBlockedBar"></span></div>
        </div>
        <div class="history" id="mediumHistory"></div>
        <div class="activity-legend"><span>Recent activity trend</span><span id="mediumTrend">Stable</span></div>
      </div>
      <div class="panel task-card">
        <div class="task-title">
          <div>
            <div class="task-name">Low Priority Task</div>
            <div class="task-sub">Lowest-priority stress participant</div>
          </div>
          <span class="badge badge-low">P1</span>
        </div>
        <div class="stats-grid">
          <div class="stat-box"><div class="stat-label">Attempts</div><div class="stat-value" id="lowAttempts">0</div></div>
          <div class="stat-box"><div class="stat-label">Successes</div><div class="stat-value" id="lowSuccesses">0</div></div>
          <div class="stat-box"><div class="stat-label">Blocked</div><div class="stat-value" id="lowBlocked">0</div></div>
          <div class="stat-box"><div class="stat-label">Last Counter</div><div class="stat-value" id="lowLast">0</div></div>
        </div>
        <div class="bar-wrap">
          <div class="bar-head"><span>Success Efficiency</span><strong id="lowRate">0%</strong></div>
          <div class="bar success"><span id="lowRateBar"></span></div>
        </div>
        <div class="bar-wrap">
          <div class="bar-head"><span>Blocking Pressure</span><strong id="lowBlockedRate">0%</strong></div>
          <div class="bar blocked"><span id="lowBlockedBar"></span></div>
        </div>
        <div class="history" id="lowHistory"></div>
        <div class="activity-legend"><span>Recent activity trend</span><span id="lowTrend">Stable</span></div>
      </div>
      <div class="panel task-card">
        <div class="task-title">
          <div>
            <div class="task-name">Shared Resource</div>
            <div class="task-sub">Global counter and lock activity</div>
          </div>
          <span class="badge" style="background:rgba(58,230,208,0.14); color:var(--accent);">RES</span>
        </div>
        <div class="stats-grid">
          <div class="stat-box"><div class="stat-label">GPIO</div><div class="stat-value">33</div></div>
          <div class="stat-box"><div class="stat-label">Current State</div><div class="stat-value" id="resourceState">IDLE</div></div>
          <div class="stat-box"><div class="stat-label">Last Owner</div><div class="stat-value" id="resourceOwner">NONE</div></div>
          <div class="stat-box"><div class="stat-label">Counter Value</div><div class="stat-value" id="resourceCounter">0</div></div>
        </div>
        <div class="bar-wrap">
          <div class="bar-head"><span>Resource Utilization</span><strong id="resourceUtilLabel">0%</strong></div>
          <div class="bar success"><span id="resourceUtilBar"></span></div>
        </div>
        <div class="bar-wrap">
          <div class="bar-head"><span>Protection Stack</span><strong id="resourceProtection">Mutex + Semaphore</strong></div>
          <div class="bar blocked"><span id="resourceProtectionBar" style="width:100%;"></span></div>
        </div>
        <div class="history" id="resourceHistory"></div>
        <div class="activity-legend"><span>Recent lock activity</span><span id="resourceTrend">Stable</span></div>
      </div>
    </section>
    <div class="refresh">
      <div style="display:flex; align-items:center; gap:10px;"><span class="tick"></span><span>Live view refreshes every second from the ESP32 local web server.</span></div>
      <div id="lastUpdated">Waiting for first update...</div>
    </div>
  </div>
  <script>
    const histories = { high: [], medium: [], low: [], resource: [] };

    function setText(id, value) {
      document.getElementById(id).textContent = value;
    }

    function setWidth(id, value) {
      document.getElementById(id).style.width = value + "%";
    }

    function setState(id, enabled) {
      const node = document.getElementById(id);
      node.textContent = enabled ? "ON" : "OFF";
      node.className = "value " + (enabled ? "status-on" : "status-off");
    }

    function toPercent(part, total) {
      if (!total) {
        return 0;
      }
      return Math.round((part / total) * 100);
    }

    function clampPercent(value) {
      return Math.max(0, Math.min(100, value));
    }

    function formatTrend(latest, previous) {
      if (previous === undefined) {
        return "Stable";
      }
      if (latest > previous) {
        return "Rising";
      }
      if (latest < previous) {
        return "Cooling";
      }
      return "Stable";
    }

    function renderHistory(id, samples, color) {
      const host = document.getElementById(id);
      host.innerHTML = "";
      const max = Math.max(1, ...samples);
      samples.forEach((value) => {
        const bar = document.createElement("span");
        const height = Math.max(8, Math.round((value / max) * 54));
        bar.style.height = height + "px";
        bar.style.background = color;
        host.appendChild(bar);
      });
    }

    function updateTask(prefix, task, historyKey, color) {
      setText(prefix + "Attempts", task.attempts);
      setText(prefix + "Successes", task.successes);
      setText(prefix + "Blocked", task.blocked);
      setText(prefix + "Last", task.lastObservedCounter);

      const successRate = clampPercent(toPercent(task.successes, task.attempts));
      const blockedRate = clampPercent(toPercent(task.blocked, task.attempts));
      setText(prefix + "Rate", successRate + "%");
      setText(prefix + "BlockedRate", blockedRate + "%");
      setWidth(prefix + "RateBar", successRate);
      setWidth(prefix + "BlockedBar", blockedRate);

      const recentValue = task.successes + task.blocked;
      const history = histories[historyKey];
      const previous = history.length ? history[history.length - 1] : undefined;
      history.push(recentValue);
      if (history.length > 16) {
        history.shift();
      }

      setText(prefix + "Trend", formatTrend(recentValue, previous));
      renderHistory(prefix + "History", history, color);
    }

    function updateResourceCard(data, totalAttempts, totalSuccesses) {
      setText("resourceState", data.resource.active ? "LOCKED" : "IDLE");
      setText("resourceOwner", data.resource.lastOwner);
      setText("resourceCounter", data.counter);
      const utilization = clampPercent(toPercent(totalSuccesses, totalAttempts || 1));
      setText("resourceUtilLabel", utilization + "%");
      setWidth("resourceUtilBar", utilization);
      setText(
        "resourceProtection",
        data.mutexProtection && data.accessSemaphore
          ? "Mutex + Semaphore"
          : data.mutexProtection
            ? "Mutex Only"
            : "Unprotected"
      );
      setWidth("resourceProtectionBar", data.mutexProtection ? (data.accessSemaphore ? 100 : 60) : 20);

      const history = histories.resource;
      const latest = data.counter;
      const previous = history.length ? history[history.length - 1] : undefined;
      history.push(latest);
      if (history.length > 16) {
        history.shift();
      }
      setText("resourceTrend", formatTrend(latest, previous));
      renderHistory("resourceHistory", history, "linear-gradient(180deg, rgba(58,230,208,0.28), rgba(58,230,208,0.98))");
    }

    async function loadMetrics() {
      try {
        const response = await fetch("/metrics");
        const data = await response.json();
        setText("counter", data.counter);
        setState("mutexState", data.mutexProtection);
        setState("semaphoreState", data.accessSemaphore);
        const totalAttempts = data.high.attempts + data.medium.attempts + data.low.attempts;
        const totalSuccesses = data.high.successes + data.medium.successes + data.low.successes;
        const totalBlocked = data.high.blocked + data.medium.blocked + data.low.blocked;
        const overallSuccessRate = clampPercent(toPercent(totalSuccesses, totalAttempts));
        const contentionRate = clampPercent(toPercent(totalBlocked, totalAttempts));

        setText("overallSuccessRate", overallSuccessRate + "%");
        setText("totalAttempts", totalAttempts);
        setText("totalBlocked", totalBlocked);
        setText("contentionLevel", contentionRate >= 30 ? "High" : contentionRate >= 15 ? "Moderate" : "Low");

        const tasks = [
          { name: "High", attempts: data.high.attempts },
          { name: "Medium", attempts: data.medium.attempts },
          { name: "Low", attempts: data.low.attempts }
        ];
        tasks.sort((a, b) => b.attempts - a.attempts);
        setText("mostActiveTask", tasks[0].name);

        setText("protectionSummary", data.mutexProtection && data.accessSemaphore ? "Dual sync active" : data.mutexProtection ? "Mutex only" : "Unsafe mode");
        setText("healthSummary", totalBlocked >= totalSuccesses / 2 ? "Heavy contention" : "Stable");

        updateTask("high", data.high, "high", "linear-gradient(180deg, rgba(255,138,91,0.32), rgba(255,138,91,0.95))");
        updateTask("medium", data.medium, "medium", "linear-gradient(180deg, rgba(255,209,102,0.28), rgba(255,209,102,0.95))");
        updateTask("low", data.low, "low", "linear-gradient(180deg, rgba(121,199,255,0.3), rgba(121,199,255,0.95))");
        updateResourceCard(data, totalAttempts, totalSuccesses);
        setText("lastUpdated", "Last updated: " + new Date().toLocaleTimeString());
      } catch (error) {
        console.error(error);
        setText("lastUpdated", "Connection retrying...");
      }
    }

    loadMetrics();
    setInterval(loadMetrics, 1000);
  </script>
</body>
</html>
    )rawliteral";

    dashboardServer.send_P(200, "text/html", html);
}

static void handleDashboardMetrics()
{
    TaskStats highCopy;
    TaskStats mediumCopy;
    TaskStats lowCopy;
    int32_t counterCopy = 0;
    bool resourceActiveCopy = false;
    const char *ownerCopy = "NONE";
    String payload;

    captureStatsSnapshot(highCopy, mediumCopy, lowCopy, counterCopy, resourceActiveCopy, ownerCopy);

    payload.reserve(512);
    payload += "{";
    payload += "\"counter\":";
    payload += String(counterCopy);
    payload += ",\"resource\":{\"active\":";
    payload += resourceActiveCopy ? "true" : "false";
    payload += ",\"lastOwner\":\"";
    payload += ownerCopy;
    payload += "\"}";
    payload += ",\"mutexProtection\":";
#if USE_MUTEX_PROTECTION
    payload += "true";
#else
    payload += "false";
#endif
    payload += ",\"accessSemaphore\":";
#if USE_ACCESS_SEMAPHORE
    payload += "true";
#else
    payload += "false";
#endif
    payload += ",\"high\":{\"attempts\":";
    payload += String(highCopy.attempts);
    payload += ",\"successes\":";
    payload += String(highCopy.successes);
    payload += ",\"blocked\":";
    payload += String(highCopy.blocked);
    payload += ",\"lastObservedCounter\":";
    payload += String(highCopy.lastObservedCounter);
    payload += "},\"medium\":{\"attempts\":";
    payload += String(mediumCopy.attempts);
    payload += ",\"successes\":";
    payload += String(mediumCopy.successes);
    payload += ",\"blocked\":";
    payload += String(mediumCopy.blocked);
    payload += ",\"lastObservedCounter\":";
    payload += String(mediumCopy.lastObservedCounter);
    payload += "},\"low\":{\"attempts\":";
    payload += String(lowCopy.attempts);
    payload += ",\"successes\":";
    payload += String(lowCopy.successes);
    payload += ",\"blocked\":";
    payload += String(lowCopy.blocked);
    payload += ",\"lastObservedCounter\":";
    payload += String(lowCopy.lastObservedCounter);
    payload += "}}";

    dashboardServer.send(200, "application/json", payload);
}

static void startDashboard()
{
    WiFi.mode(WIFI_AP);
    WiFi.softAP(DASHBOARD_AP_SSID, DASHBOARD_AP_PASSWORD);

    dashboardServer.on("/", HTTP_GET, handleDashboardRoot);
    dashboardServer.on("/metrics", HTTP_GET, handleDashboardMetrics);
    dashboardServer.begin();

    Serial.println("Local dashboard started.");
    Serial.printf("SSID: %s\r\n", DASHBOARD_AP_SSID);
    Serial.printf("Password: %s\r\n", DASHBOARD_AP_PASSWORD);
    Serial.printf("Open: http://%s/\r\n", WiFi.softAPIP().toString().c_str());
}

void setup()
{
    Serial.begin(SERIAL_BAUD_RATE);
    delay(500);

    randomSeed(static_cast<uint32_t>(esp_random()));

    pinMode(RESOURCE_LED_PIN, OUTPUT);
    digitalWrite(RESOURCE_LED_PIN, LOW);

#if USE_MUTEX_PROTECTION
    resourceMutex = xSemaphoreCreateMutex();
    if (resourceMutex == nullptr)
    {
        Serial.println("Fatal: failed to create resource mutex.");
        return;
    }
#endif

#if USE_ACCESS_SEMAPHORE
    accessSemaphore = xSemaphoreCreateBinary();
    if (accessSemaphore == nullptr)
    {
        Serial.println("Fatal: failed to create access semaphore.");
        return;
    }
    xSemaphoreGive(accessSemaphore);
#endif

    Serial.println("ESP32 Shared Resource Stress Test Starting...");
    startDashboard();

    const BaseType_t highCreated =
        xTaskCreatePinnedToCore(stressTask, "HighTask", TASK_STACK_SIZE, (void *)&HIGH_TASK, HIGH_TASK.priority, nullptr, 1);
    const BaseType_t mediumCreated =
        xTaskCreatePinnedToCore(stressTask, "MediumTask", TASK_STACK_SIZE, (void *)&MEDIUM_TASK, MEDIUM_TASK.priority, nullptr, 1);
    const BaseType_t lowCreated =
        xTaskCreatePinnedToCore(stressTask, "LowTask", TASK_STACK_SIZE, (void *)&LOW_TASK, LOW_TASK.priority, nullptr, 1);
    const BaseType_t metricsCreated =
        xTaskCreatePinnedToCore(metricsTask, "MetricsTask", TASK_STACK_SIZE, nullptr, 1, nullptr, 0);

    systemReady =
        highCreated == pdPASS &&
        mediumCreated == pdPASS &&
        lowCreated == pdPASS &&
        metricsCreated == pdPASS;

    if (!systemReady)
    {
        Serial.println("Fatal: one or more FreeRTOS tasks could not be created.");
    }
}

void loop()
{
    if (systemReady)
    {
        dashboardServer.handleClient();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}
