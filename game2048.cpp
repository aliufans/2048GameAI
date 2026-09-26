// ============================================================================
//  game2048.cpp —— 2048 小游戏（C++ / Win32 GDI 原生界面）+ 蛇形算法 AI
//
//  功能：
//    * 启动即初始化界面，显示「开始游戏」按钮
//    * 开始前可选择棋盘大小 3×3 / 4×4 / 5×5
//    * 方向键 或 WASD 操控
//    * 实时显示当前分数、操控步数、最大方块、目标值
//    * 撤回按钮（可连续回退，撤销栈上限 1000 步）
//    * 内置蛇形算法 AI，游戏中可随时开启 / 关闭
//    * AI 速度用滑块实时调节
//
//  编译（任选其一）：
//    zig c++ -O2 -std=c++17 -o game2048.exe game2048.cpp -mwindows -lgdi32 -luser32 -static
//    g++     -O2 -std=c++17 -o game2048.exe game2048.cpp -mwindows -lgdi32 -luser32 -static
//    MSVC:   cl /O2 /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE game2048.cpp
//            user32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS
//
//  命令行（图形版和压测版都支持，压测结果同时写入 bench_result.txt）：
//    game2048.exe --bench [边长] [局数]      # 无界面压测 AI（默认 4×4 打 30 局）
//    game2048.exe --sweep [边长] [每组局数]  # 参数扫描，对比不同评估参数的达标率
//    --think <毫秒>   每步思考时间上限（默认 30）
//    --base <底数>    蛇形权重底数（默认 2）
//    --empty <系数>   空格奖励系数（默认 8）
//    --merge <系数>   可合并相邻对奖励（默认 0，实测加上会变差）
//    --cutoff <概率>  概率剪枝阈值（默认 1e-4）
//    --depth <层数>   >0 时固定搜索层数（做对照实验用）
// ============================================================================

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------- 基本常量

static const int MAXN = 5;                   // 最大棋盘边长
static const int MAXCELLS = MAXN * MAXN;     // 最多 25 格

enum Dir { DIR_UP = 0, DIR_DOWN = 1, DIR_LEFT = 2, DIR_RIGHT = 3 };

static const int PHASE_SETUP = 0;            // 未开始（显示开始按钮）
static const int PHASE_RUNNING = 1;
static const int PHASE_OVER = 2;

// 3×3 只有 9 格，理论极限只有 1024（2^(9+1)，且需要近乎完美的操作），2048 根本无法出现，
// 实测本 AI 平均能到 256 左右，所以目标值自动下调为 256；4×4、5×5 的目标都是 2048。
static int targetForSize(int n) { return (n == 3) ? 256 : 2048; }

// ---- 生成方块模式（配置里可切换，用于测试） ----
static const int SPAWN_RANDOM = 0;   // 90% 出 2、10% 出 4（正常玩法）
static const int SPAWN_ONLY2 = 1;    // 全 2 模式：只生成 2
static const int SPAWN_ONLY4 = 2;    // 全 4 模式：只生成 4
static int g_spawnMode = SPAWN_RANDOM;

// 每步思考时间上限（毫秒），可用 --think 覆盖
static double g_thinkBudgetMs = 30.0;

// ---------------------------------------------------------------- 棋盘

struct Grid {
    int v[MAXCELLS];
};

static inline int GIDX(int n, int r, int c) { return r * n + c; }

static inline void gridClear(Grid& g, int n) {
    for (int i = 0; i < n * n; ++i) g.v[i] = 0;
}

static bool gridEqual(const Grid& a, const Grid& b, int n) {
    for (int i = 0; i < n * n; ++i)
        if (a.v[i] != b.v[i]) return false;
    return true;
}

static int gridMax(const Grid& g, int n) {
    int m = 0;
    for (int i = 0; i < n * n; ++i)
        if (g.v[i] > m) m = g.v[i];
    return m;
}

static int emptyCellsOf(const Grid& g, int n, int* out) {
    int cnt = 0;
    for (int i = 0; i < n * n; ++i)
        if (!g.v[i]) out[cnt++] = i;
    return cnt;
}

// 90% 出 2，10% 出 4；「全2 / 全4 模式」下只出指定数值（用于测试）
static bool spawnTile(Grid& g, int n, std::mt19937& rng) {
    int cells[MAXCELLS];
    int cnt = emptyCellsOf(g, n, cells);
    if (!cnt) return false;
    std::uniform_int_distribution<int> pick(0, cnt - 1);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    int value;
    if (g_spawnMode == SPAWN_ONLY2) value = 2;
    else if (g_spawnMode == SPAWN_ONLY4) value = 4;
    else value = (uni(rng) < 0.1) ? 4 : 2;
    g.v[cells[pick(rng)]] = value;
    return true;
}

// 把一条线朝索引 0 方向压缩合并，返回本线得分（每个方块一次移动只合并一次）
static int mergeLine(int* line, int len) {
    int vals[MAXN + 2];
    int m = 0;
    for (int i = 0; i < len; ++i)
        if (line[i]) vals[m++] = line[i];

    int out[MAXN + 2];
    int k = 0, gained = 0;
    for (int i = 0; i < m;) {
        if (i + 1 < m && vals[i] == vals[i + 1]) {
            int val = vals[i] * 2;
            out[k++] = val;
            gained += val;
            i += 2;
        } else {
            out[k++] = vals[i++];
        }
    }
    for (int i = 0; i < len; ++i) line[i] = (i < k) ? out[i] : 0;
    return gained;
}

static int moveGrid(const Grid& src, int n, int dir, Grid& dst) {
    dst = src;
    int line[MAXN + 2];
    int gained = 0;

    if (dir == DIR_LEFT || dir == DIR_RIGHT) {
        for (int r = 0; r < n; ++r) {
            for (int c = 0; c < n; ++c) line[c] = src.v[GIDX(n, r, c)];
            if (dir == DIR_RIGHT) std::reverse(line, line + n);
            gained += mergeLine(line, n);
            if (dir == DIR_RIGHT) std::reverse(line, line + n);
            for (int c = 0; c < n; ++c) dst.v[GIDX(n, r, c)] = line[c];
        }
    } else {
        for (int c = 0; c < n; ++c) {
            for (int r = 0; r < n; ++r) line[r] = src.v[GIDX(n, r, c)];
            if (dir == DIR_DOWN) std::reverse(line, line + n);
            gained += mergeLine(line, n);
            if (dir == DIR_DOWN) std::reverse(line, line + n);
            for (int r = 0; r < n; ++r) dst.v[GIDX(n, r, c)] = line[r];
        }
    }
    return gained;
}

static bool isGameOver(const Grid& g, int n) {
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            int v = g.v[GIDX(n, r, c)];
            if (!v) return false;
            if (c + 1 < n && v == g.v[GIDX(n, r, c + 1)]) return false;
            if (r + 1 < n && v == g.v[GIDX(n, r + 1, c)]) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- 一局游戏

struct Snapshot {
    Grid grid;
    int score;
    int moves;
};

struct Game {
    int n = 4;
    int target = 2048;
    Grid grid{};
    int score = 0;
    int moves = 0;
    bool over = false;
    bool reached = false;
    std::vector<Snapshot> history;

    void reset(int size) {
        n = size;
        target = targetForSize(size);
        gridClear(grid, n);
        score = 0;
        moves = 0;
        over = false;
        reached = false;
        history.clear();
    }

    // 走一步；无效移动返回 false 且不计步数
    bool doMove(int dir, std::mt19937& rng, int* gainedOut = nullptr) {
        if (over) return false;
        Grid nxt;
        int gained = moveGrid(grid, n, dir, nxt);
        if (gridEqual(nxt, grid, n)) return false;

        history.push_back({grid, score, moves});
        if (history.size() > 1000) history.erase(history.begin());

        grid = nxt;
        score += gained;
        ++moves;
        spawnTile(grid, n, rng);

        if (gridMax(grid, n) >= target) reached = true;
        over = isGameOver(grid, n);
        if (gainedOut) *gainedOut = gained;
        return true;
    }

    bool canUndo() const { return !history.empty(); }

    bool undo() {
        if (history.empty()) return false;
        const Snapshot& s = history.back();
        grid = s.grid;
        score = s.score;
        moves = s.moves;
        history.pop_back();
        over = false;
        reached = gridMax(grid, n) >= target;
        return true;
    }
};

// ============================================================================
//  二、蛇形（Snake）算法 AI
// ----------------------------------------------------------------------------
//  核心思想：把棋盘所有格子按「蛇形路径」串成一维序列，越靠前的格子权重越大，
//  权重按 4 的幂次指数衰减：4^(N-1), 4^(N-2), ... , 4^1, 4^0。
//  评估值 = Σ(格子数值 × 该格权重)，于是 AI 会自发形成
//  「最大方块压在角落 + 数值沿蛇形路径单调递减」的阵型 —— 这正是 2048
//  能长期存活、稳定合出 2048 / 4096 的经典形态。
//
//  4×4 的蛇形权重（数字越大越重要，路径 15 → 0）：
//       15 14 13 12
//        8  9 10 11
//        7  6  5  4
//        0  1  2  3
//
//  只看当前局面容易短视，因此在蛇形评估之上再套「期望极大极小」搜索：
//    MAX 节点    → 我方在 4 个方向里选评估最高的一步
//    CHANCE 节点 → 新方块的位置/数值取数学期望（90% 出 2，10% 出 4）
//  再外面套「迭代加深 + 思考时间预算」：默认每步最多想 30ms，想得越深越准，
//  追求「每局都能合出目标方块」。
// ============================================================================

static const double SPAWN_FOUR_P = 0.1;     // 新方块出 4 的概率
static const double NEG_INF = -1e300;

// ---- 可调参数（默认值经压测调优，可用命令行覆盖以便做参数扫描） ----
static double g_snakeBase = 2.0;      // 蛇形权重底数（2 或 4，越大越强调严格蛇形）
static double g_emptyBonus = 8.0;     // 每个空格的奖励（以最大权重 w0 为单位）
static double g_mergeBonus = 0.0;     // 每个可合并相邻对的奖励（以 w0 为单位）
static double g_probCutoff = 1e-4;    // 概率剪枝阈值：累计概率低于它的分支直接估值
static bool g_useSpawn4 = true;       // 搜索时是否展开「生成 4」分支（关掉可搜得更深）

// 当前生成模式下的概率：全2/全4 模式要让 AI 知道「新方块必是 2/4」，否则等于算错模型
static inline void spawnProbs(double& p2, double& p4) {
    p2 = 1.0 - SPAWN_FOUR_P;
    p4 = SPAWN_FOUR_P;
    if (g_spawnMode == SPAWN_ONLY2) { p2 = 1.0; p4 = 0.0; }
    else if (g_spawnMode == SPAWN_ONLY4) { p2 = 0.0; p4 = 1.0; }
}
static int g_fixedDepth = 0;          // >0 时用固定深度（便于做对比实验）
static int g_lastDepth = 0;           // 上一步实际搜到的层数（统计用）
static int g_threads = 0;             // 搜索线程数（0 = 自动用满本机所有核心）

// 0 表示自动取全部核心
static int effectiveThreads() {
    if (g_threads > 0) return g_threads;
    unsigned c = std::thread::hardware_concurrency();
    return c ? (int)c : 1;
}

// ---- 工作线程池 ----
// 每层搜索都新建线程的话，12 线程光线程创建就要 ~1ms，浅层搜索根本划不来；
// 这里一次性建好线程，之后每层用「条件变量 + 任务代数」唤醒它们干活。
class ThreadPool {
public:
    void ensure(int n) {
        if (n < 1) n = 1;
        if (n == count_) return;
        stopAll();
        if (n <= 1) { count_ = 1; return; }
        count_ = n;
        stop_ = false;
        workers_.reserve((size_t)n);
        for (int i = 0; i < n; ++i) workers_.emplace_back([this, i]() { workerLoop(i); });
    }

    int count() const { return count_; }

    // 把 job(tid) 分发给所有线程并等它们全部跑完
    void run(const std::function<void(int)>& job) {
        if (count_ <= 1) { job(0); return; }
        {
            std::unique_lock<std::mutex> lk(m_);
            job_ = job;
            ++gen_;
            done_ = 0;
            active_ = count_;
        }
        cvStart_.notify_all();
        std::unique_lock<std::mutex> lk(m_);
        cvDone_.wait(lk, [this] { return done_ >= active_; });
        job_ = nullptr;
    }

    void stopAll() {
        if (!workers_.empty()) {
            {
                std::unique_lock<std::mutex> lk(m_);
                stop_ = true;
            }
            cvStart_.notify_all();
            for (auto& t : workers_) {
                if (t.joinable()) t.join();
            }
            workers_.clear();
        }
        count_ = 0;
        stop_ = false;
    }

private:
    void workerLoop(int tid) {
        long long seen = 0;
        for (;;) {
            std::function<void(int)> f;
            {
                std::unique_lock<std::mutex> lk(m_);
                cvStart_.wait(lk, [this, &seen] { return stop_ || gen_ != seen; });
                if (stop_) return;
                seen = gen_;
                f = job_;
            }
            if (f) f(tid);
            {
                std::lock_guard<std::mutex> lk(m_);
                ++done_;
                if (done_ >= active_) cvDone_.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cvStart_, cvDone_;
    std::function<void(int)> job_;
    long long gen_ = 0;
    int done_ = 0, active_ = 0, count_ = 0;
    bool stop_ = false;
};

static ThreadPool g_pool;

// ---- AI 深度档（游戏界面里可选；最高档会把所有 CPU 核心吃满） ----
struct AiLevel {
    const wchar_t* name;   // 界面显示名
    int depth;             // 固定搜索层数
    double cutoff;         // 概率剪枝阈值（越小搜得越狠）
    int threads;           // 线程数，0 = 用满本机所有核心
    double budgetMs;       // 单步思考时间上限
};

// 有线程池兜底，并行开销很小，所以各档都用满核心；「快」档层数 ≤3 不会走并行路径。
// 层数越高每步越久（4×4 开阔局面实测：4 层≈20ms、5 层(1e-5)≈1.3s、5 层(1e-7)≈8s、6 层≈3s 起）。
// 深档的剪枝要比标准档松（1e-7），否则剪枝自己会把树截断在 5 层左右，标称深度就名不副实了。
static const AiLevel AI_LEVELS[6] = {
    {L"快·3层",   3, 1e-4, 0,    30.0},
    {L"标准·4层", 4, 1e-4, 0,   300.0},
    {L"深·5层",   5, 1e-5, 0,  3000.0},
    {L"更深·6层", 6, 1e-7, 0, 30000.0},
    {L"极限·7层", 7, 1e-7, 0, 90000.0},
    {L"满负荷·自适应", 0, 1e-5, 0, 5000.0},   // depth=0 表示自适应加深
};
static const int AI_LEVEL_COUNT = 6;
static const int AI_LEVEL_LOAD_ALL = 5;    // 「满负荷」档的下标

// ---- 蛇形权重（按边长缓存） ----
static double s_weightCache[MAXN + 1][MAXCELLS];
static bool s_weightBuilt[MAXN + 1] = {false, false, false, false, false, false};
static double s_weightBuiltBase[MAXN + 1] = {0, 0, 0, 0, 0, 0};

static const double* snakeWeights(int n) {
    if (s_weightBuilt[n] && s_weightBuiltBase[n] == g_snakeBase) return s_weightCache[n];
    // 生成蛇形路径：偶数行向右，奇数行向左，起点取左上角
    int path[MAXCELLS];
    int k = 0;
    for (int r = 0; r < n; ++r) {
        if (r % 2 == 0) {
            for (int c = 0; c < n; ++c) path[k++] = GIDX(n, r, c);
        } else {
            for (int c = n - 1; c >= 0; --c) path[k++] = GIDX(n, r, c);
        }
    }
    const int total = n * n;
    for (int i = 0; i < n * n; ++i) s_weightCache[n][i] = 0.0;
    for (int i = 0; i < total; ++i) {
        double w = 1.0;
        for (int e = 0; e < total - 1 - i; ++e) w *= g_snakeBase;   // base^(total-1-i)
        s_weightCache[n][path[i]] = w;
    }
    s_weightBuilt[n] = true;
    s_weightBuiltBase[n] = g_snakeBase;
    return s_weightCache[n];
}

// 提前把各尺寸的蛇形权重算好（搜索是多线程的，不能在搜索过程中懒加载）
static void warmUpWeights() {
    for (int n = 3; n <= MAXN; ++n) snakeWeights(n);
}

// ---- 评估函数 ----
static double evalGrid(const Grid& g, int n) {
    const double* w = snakeWeights(n);
    double total = 0.0;
    int empties = 0;
    int merges = 0;
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            const int i = GIDX(n, r, c);
            int v = g.v[i];
            if (v) total += (double)v * w[i];
            else ++empties;
            if (g_mergeBonus > 0.0 && v) {
                if (c + 1 < n && v == g.v[GIDX(n, r, c + 1)]) ++merges;
                if (r + 1 < n && v == g.v[GIDX(n, r + 1, c)]) ++merges;
            }
        }
    }
    total += (double)empties * w[0] * g_emptyBonus;
    if (merges) total += (double)merges * w[0] * g_mergeBonus;
    return total;
}

// ---- 搜索上下文（节点计数 + 时间预算） ----
struct SearchCtx {
    int n = 4;
    long long nodes = 0;
    bool aborted = false;
    std::chrono::steady_clock::time_point t0;
    double budgetMs = 30.0;

    double elapsedMs() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    // 每 512 个节点检查一次时间，超时就整体放弃这一层
    inline void tick() {
        if ((++nodes & 511) == 0 && elapsedMs() > budgetMs) aborted = true;
    }
};

static double searchChance(SearchCtx& ctx, const Grid& g, int depth, double prob);

// MAX 节点：我方选最优方向
static double searchMax(SearchCtx& ctx, const Grid& g, int depth, double prob) {
    if (depth <= 0 || prob < g_probCutoff) return evalGrid(g, ctx.n);
    ctx.tick();
    if (ctx.aborted) return 0.0;

    double best = NEG_INF;
    Grid nxt;
    for (int d = 0; d < 4; ++d) {
        moveGrid(g, ctx.n, d, nxt);
        if (gridEqual(nxt, g, ctx.n)) continue;          // 该方向没有位移
        double v = searchChance(ctx, nxt, depth, prob);
        if (ctx.aborted) return 0.0;
        if (v > best) best = v;
    }
    if (best == NEG_INF) return evalGrid(g, ctx.n) - 1e18;    // 死局
    return best;
}

// CHANCE 节点：对新方块的位置与数值取期望
static double searchChance(SearchCtx& ctx, const Grid& g, int depth, double prob) {
    if (depth <= 0 || prob < g_probCutoff) return evalGrid(g, ctx.n);

    int cells[MAXCELLS];
    int cnt = emptyCellsOf(g, ctx.n, cells);
    if (!cnt) return searchMax(ctx, g, depth - 1, prob);      // 棋盘已满，不生成新方块

    ctx.tick();
    if (ctx.aborted) return 0.0;

    const double pEach = 1.0 / (double)cnt;
    double sv2, sv4;
    spawnProbs(sv2, sv4);
    double total = 0.0;
    for (int k = 0; k < cnt; ++k) {
        Grid probe = g;
        if (sv2 > 0.0) {
            probe.v[cells[k]] = 2;
            total += sv2 * searchMax(ctx, probe, depth - 1, prob * pEach * sv2);
        }
        if (sv4 > 0.0 && g_useSpawn4) {
            probe.v[cells[k]] = 4;
            total += sv4 * searchMax(ctx, probe, depth - 1, prob * pEach * sv4);
        }
        if (ctx.aborted) return 0.0;
    }
    return total * pEach;
}

// ---- 根节点：迭代加深，返回最佳方向 ----
struct AiChoice {
    int dir = -1;
    int depth = 0;
    double ms = 0.0;
};

// 浮点近似相等（用于「同分」判定，避免累加顺序造成的极小误差）
static bool nearlyEqual(double a, double b) {
    double d = std::fabs(a - b);
    double m = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
    return d <= 1e-9 * m;
}

// ---- 根节点一层的搜索（可按「方向 × 空格」拆任务多线程并行） ----
struct RootLayer {
    double val[4] = {NEG_INF, NEG_INF, NEG_INF, NEG_INF};
    bool moved[4] = {false, false, false, false};
    bool have = false;                    // 是否算出了可用结果
    bool aborted = false;                 // 超时或被取消
    double ms = 0.0;
};

static void evalRootLayer(const Grid& g, int n, int depth, double budgetMs,
                          RootLayer& out, const std::atomic<bool>* cancelFlag) {
    out = RootLayer{};
    auto t0 = std::chrono::steady_clock::now();

    // 1) 先算出每个方向的落子结果与「方向 × 空格」任务表
    Grid after[4];
    int cells[4][MAXCELLS];
    int cnt[4] = {0, 0, 0, 0};

    struct Task { int dir; int cell; };
    std::vector<Task> tasks;
    tasks.reserve(4 * MAXCELLS);

    for (int d = 0; d < 4; ++d) {
        moveGrid(g, n, d, after[d]);
        out.moved[d] = !gridEqual(after[d], g, n);
        if (!out.moved[d]) continue;
        cnt[d] = emptyCellsOf(after[d], n, cells[d]);
        if (cnt[d] == 0) {
            tasks.push_back({d, -1});                 // 棋盘已满：不生成新方块
        } else {
            for (int k = 0; k < cnt[d]; ++k) tasks.push_back({d, k});
        }
    }
    if (tasks.empty()) return;

    // 2) 每个任务都是一棵规模相近的子树，均分给 N 个线程即可负载均衡。
    //    浅层（depth<4）本身只有几十微秒，开线程反而更慢，直接串行。
    int T = effectiveThreads();
    if ((int)tasks.size() < T) T = (int)tasks.size();
    const bool usePool = (depth >= 4) && (T > 1);
    if (usePool) {
        g_pool.ensure(T);
        T = g_pool.count();
        if (T < 1) T = 1;
    } else {
        T = 1;
    }

    double sv2, sv4;
    spawnProbs(sv2, sv4);

    std::vector<double> part((size_t)T * 4, 0.0);
    std::atomic<int> nextIdx{0};
    std::atomic<bool> aborted{false};

    auto runTasks = [&](int tid) {
        SearchCtx ctx;
        ctx.n = n;
        ctx.t0 = t0;
        ctx.budgetMs = budgetMs;
        double* acc = &part[(size_t)tid * 4];
        for (;;) {
            if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) {
                aborted.store(true);
                return;
            }
            const int i = nextIdx.fetch_add(1, std::memory_order_relaxed);
            if (i >= (int)tasks.size()) return;

            const Task t = tasks[(size_t)i];
            const double pEach = (cnt[t.dir] > 0) ? 1.0 / (double)cnt[t.dir] : 1.0;
            if (t.cell < 0) {
                acc[t.dir] += searchMax(ctx, after[t.dir], depth - 1, 1.0);
            } else {
                Grid probe = after[t.dir];
                if (sv2 > 0.0) {
                    probe.v[cells[t.dir][t.cell]] = 2;
                    acc[t.dir] += sv2 * searchMax(ctx, probe, depth - 1, pEach * sv2);
                }
                if (sv4 > 0.0 && g_useSpawn4) {
                    probe.v[cells[t.dir][t.cell]] = 4;
                    acc[t.dir] += sv4 * searchMax(ctx, probe, depth - 1, pEach * sv4);
                }
            }
            if (ctx.aborted) {
                aborted.store(true);
                return;
            }
        }
    };

    if (T > 1) {
        g_pool.run(runTasks);
    } else {
        runTasks(0);
    }

    out.aborted = aborted.load();
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (out.aborted) return;

    for (int d = 0; d < 4; ++d) {
        if (!out.moved[d]) continue;
        double total = 0.0;
        for (int t = 0; t < T; ++t) total += part[(size_t)t * 4 + d];
        const double pEach = (cnt[d] > 0) ? 1.0 / (double)cnt[d] : 1.0;
        out.val[d] = total * pEach;
        out.have = true;
    }
}

static AiChoice chooseMove(const Grid& g, int n, std::mt19937& rng,
                           const std::atomic<bool>* cancelFlag = nullptr) {
    AiChoice res;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    // 第一层固定用最小的 2 层：它极其便宜，保证任何预算下都能拿到可用结果；
    // 之后逐层加深，只有「完整搜完」的层才会被采纳。
    const int kStartDepth = 2;
    const int kMaxDepth = (g_fixedDepth > 0) ? g_fixedDepth : ((n >= 5) ? 6 : 8);
    const bool adaptive = (g_fixedDepth <= 0);
    (void)adaptive;

    int bestDirs[4] = {0, 0, 0, 0};
    int bestCount = 0;

    // 下一层大致要花「本层耗时 × 2×空格数」（每层分支倍数），据此判断值不值得加深
    int rootCells[MAXCELLS];
    const int rootEmpties = emptyCellsOf(g, n, rootCells);
    const double growth = std::max(4.0, 2.0 * (double)rootEmpties);
    double prevElapsed = 0.0;

    for (int depth = kStartDepth; depth <= kMaxDepth; ++depth) {
        const double remain = std::max(1.0, g_thinkBudgetMs - prevElapsed);
        RootLayer layer;
        evalRootLayer(g, n, depth, remain, layer, cancelFlag);
        if (layer.aborted || !layer.have) break;
        if (cancelFlag && cancelFlag->load()) break;

        double layerVal = NEG_INF;
        int layerDirs[4] = {0, 0, 0, 0};
        int layerCount = 0;
        for (int d = 0; d < 4; ++d) {
            if (!layer.moved[d]) continue;
            const double v = layer.val[d];
            if (v > layerVal + 1e-6) {
                layerVal = v;
                layerCount = 0;
                layerDirs[layerCount++] = d;
            } else if (nearlyEqual(v, layerVal) && layerCount < 4) {
                layerDirs[layerCount++] = d;
            }
        }
        if (layerCount == 0) break;

        bestCount = layerCount;
        for (int i = 0; i < layerCount; ++i) bestDirs[i] = layerDirs[i];
        res.depth = depth;

        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        const double layerCost = elapsed - prevElapsed;
        prevElapsed = elapsed;
        if (elapsed >= g_thinkBudgetMs) break;
        // 预估下一层耗时，超出剩余预算就收工（所有档位都生效，省得白算一场再被超时丢弃）
        if (layerCost * growth > g_thinkBudgetMs - elapsed) break;
    }
    g_lastDepth = res.depth;

    if (bestCount == 0) {
        // 极端情况：连 2 层都没搜完（预算被压到极小），退化成 1 步贪心，绝不乱走
        SearchCtx quick;
        quick.n = n;
        quick.t0 = std::chrono::steady_clock::now();
        quick.budgetMs = 1e9;                     // 这一层不加时间限制
        Grid nxt;
        double bestGreedy = NEG_INF;
        for (int d = 0; d < 4; ++d) {
            moveGrid(g, n, d, nxt);
            if (gridEqual(nxt, g, n)) continue;
            double v = searchChance(quick, nxt, 1, 1.0);
            if (v > bestGreedy + 1e-6) {
                bestGreedy = v;
                bestCount = 0;
                bestDirs[bestCount++] = d;
            } else if (nearlyEqual(v, bestGreedy) && bestCount < 4) {
                bestDirs[bestCount++] = d;
            }
        }
        res.depth = 1;
    }
    if (bestCount == 0) {
        res.dir = -1;
    } else if (bestCount == 1) {
        res.dir = bestDirs[0];
    } else {
        std::uniform_int_distribution<int> pick(0, bestCount - 1);
        res.dir = bestDirs[pick(rng)];        // 同分随机打散，避免固定套路
    }
    res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return res;
}

// ---- 无界面压测：验证「AI 必得 2048」 ----
struct BenchStats {
    int games = 0, ok = 0, bestTile = 0;
    int cnt256 = 0, cnt512 = 0, cnt1024 = 0, cnt2048 = 0;
    double avgScore = 0, avgMoves = 0, avgThink = 0, avgDepth = 0, avgMaxTile = 0, wallSec = 0;
};

static BenchStats benchRun(int size, int games, unsigned seedBase, FILE* fp, bool verbose) {
    BenchStats st;
    const int target = targetForSize(size);
    st.games = games;
    warmUpWeights();

    double totalScore = 0.0, totalMoves = 0.0, totalThink = 0.0, totalDepth = 0.0, totalMax = 0.0;
    long long thinkCount = 0;

    auto wall0 = std::chrono::steady_clock::now();
    for (int i = 0; i < games; ++i) {
        std::mt19937 rng(seedBase + (unsigned)i * 7919u);
        Game g;
        g.reset(size);
        spawnTile(g.grid, size, rng);
        spawnTile(g.grid, size, rng);

        double thinkSum = 0.0;
        int depthSum = 0;
        while (!g.over) {
            AiChoice c = chooseMove(g.grid, size, rng);
            thinkSum += c.ms;
            depthSum += g_lastDepth;
            if (c.dir < 0) break;
            if (!g.doMove(c.dir, rng)) break;
        }
        const int moves = g.moves ? g.moves : 1;
        totalThink += thinkSum;
        totalDepth += (double)depthSum / moves;
        thinkCount += moves;

        int mt = gridMax(g.grid, size);
        if (mt > st.bestTile) st.bestTile = mt;
        totalScore += g.score;
        totalMoves += g.moves;
        totalMax += mt;
        if (mt >= 256) ++st.cnt256;
        if (mt >= 512) ++st.cnt512;
        if (mt >= 1024) ++st.cnt1024;
        if (mt >= 2048) ++st.cnt2048;
        if (mt >= target) ++st.ok;

        if (verbose && fp) {
            std::fprintf(fp, "  第 %2d 局：最大方块 %6d  分数 %7d  步数 %5d  平均思考 %.1fms  平均深度 %.1f  %s\n",
                         i + 1, mt, g.score, g.moves, thinkSum / moves,
                         (double)depthSum / moves, (mt >= target) ? "[达标]" : "[未达标]");
            std::fflush(fp);
        }
    }
    st.wallSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    st.avgScore = totalScore / (games ? games : 1);
    st.avgMoves = totalMoves / (games ? games : 1);
    st.avgMaxTile = totalMax / (games ? games : 1);
    st.avgThink = totalThink / (thinkCount ? thinkCount : 1);
    st.avgDepth = totalDepth / (games ? games : 1);
    return st;
}

static void runBench(int size, int games, const wchar_t* outPath) {
    FILE* fp = stdout;
    bool toFile = false;
    if (outPath && *outPath) {
        FILE* f = _wfopen(outPath, L"w");
        if (f) { fp = f; toFile = true; }
    }

    std::fprintf(fp, "棋盘 %d×%d  目标 %d  思考上限 %.0fms/步  蛇形底数 %.0f  空格奖励 %.1f\n",
                 size, size, targetForSize(size), g_thinkBudgetMs, g_snakeBase, g_emptyBonus);
    std::fprintf(fp, "%s\n", "----------------------------------------------------------------");

    BenchStats st = benchRun(size, games, 20260926u, fp, true);

    std::fprintf(fp, "%s\n", "----------------------------------------------------------------");
    std::fprintf(fp, "达标率：%d/%d = %.1f%%\n", st.ok, st.games, 100.0 * st.ok / (st.games ? st.games : 1));
    std::fprintf(fp, "历史最大方块：%d  平均最大方块：%.0f\n", st.bestTile, st.avgMaxTile);
    std::fprintf(fp, "达到 512/1024/2048 的局数：%d / %d / %d\n", st.cnt512, st.cnt1024, st.cnt2048);
    std::fprintf(fp, "平均分数：%.0f   平均步数：%.0f   平均搜索深度：%.1f\n",
                 st.avgScore, st.avgMoves, st.avgDepth);
    std::fprintf(fp, "平均每步思考：%.2fms   总耗时：%.1fs\n", st.avgThink, st.wallSec);

    if (toFile) {
        std::fclose(fp);
        std::printf("压测结果已写入：%ls\n", outPath);
        std::fflush(stdout);
    }
}

// ---- 参数扫描：对比不同评估参数 / 搜索深度的达标率 ----
static void runSweep(int size, int games) {
    struct Cfg {
        const char* name;
        double base, empty, merge, cutoff;
        int depth;
    };

    // 蛇形权重的跨度随棋盘变大而指数增长，空格奖励要按尺寸分别标定
    const Cfg cfg4[] = {
        {"底数2 空格5   深度4", 2.0, 5.0, 0.0, 0.0, 4},
        {"底数2 空格8   深度4", 2.0, 8.0, 0.0, 0.0, 4},
        {"底数2 空格12  深度4", 2.0, 12.0, 0.0, 0.0, 4},
        {"底数2 空格16  深度4", 2.0, 16.0, 0.0, 0.0, 4},
    };
    const Cfg cfg3[] = {
        {"底数2 空格0.5 深度6", 2.0, 0.5, 0.0, 0.0, 6},
        {"底数2 空格1   深度6", 2.0, 1.0, 0.0, 0.0, 6},
        {"底数2 空格2   深度6", 2.0, 2.0, 0.0, 0.0, 6},
        {"底数2 空格4   深度6", 2.0, 4.0, 0.0, 0.0, 6},
        {"底数2 空格8   深度6", 2.0, 8.0, 0.0, 0.0, 6},
        {"底数1.5 空格2 深度6", 1.5, 2.0, 0.0, 0.0, 6},
    };
    const Cfg cfg5[] = {
        {"底数2 空格8   深度4", 2.0, 8.0, 0.0, 0.0, 4},
        {"底数2 空格16  深度4", 2.0, 16.0, 0.0, 0.0, 4},
        {"底数2 空格32  深度4", 2.0, 32.0, 0.0, 0.0, 4},
        {"底数2 空格64  深度4", 2.0, 64.0, 0.0, 0.0, 4},
    };

    const Cfg* cfgs = cfg4;
    int ncfg = (int)(sizeof(cfg4) / sizeof(cfg4[0]));
    if (size == 3) { cfgs = cfg3; ncfg = (int)(sizeof(cfg3) / sizeof(cfg3[0])); }
    if (size == 5) { cfgs = cfg5; ncfg = (int)(sizeof(cfg5) / sizeof(cfg5[0])); }

    std::printf("参数扫描：%d×%d，每组 %d 局（目标 %d）\n", size, size, games, targetForSize(size));
    std::printf("%-24s %7s %8s %8s %8s %7s %7s %8s\n",
                "配置", "达标率", "平均最大块", "最优块", "平均步数", "512率", "思考ms", "平均深度");
    std::fflush(stdout);

    for (int i = 0; i < ncfg; ++i) {
        g_snakeBase = cfgs[i].base;
        g_emptyBonus = cfgs[i].empty;
        g_mergeBonus = cfgs[i].merge;
        g_probCutoff = cfgs[i].cutoff;
        g_fixedDepth = cfgs[i].depth;
        g_thinkBudgetMs = 30.0;

        BenchStats st = benchRun(size, games, 20260926u, nullptr, false);
        const double g_ = (double)(st.games ? st.games : 1);
        std::printf("%-24s %6.1f%% %8.0f %8d %8.0f %6.0f%% %7.2f %8.1f   (%.0fs)\n",
                    cfgs[i].name, 100.0 * st.ok / g_, st.avgMaxTile, st.bestTile,
                    st.avgMoves, 100.0 * st.cnt512 / g_, st.avgThink, st.avgDepth, st.wallSec);
        std::fflush(stdout);
    }

    // 还原默认
    g_snakeBase = 2.0;
    g_emptyBonus = 8.0;
    g_mergeBonus = 0.0;
    g_probCutoff = 1e-4;
    g_fixedDepth = 0;
}


// ============================================================================
//  三、AI 落子速度（毫秒）——滑块 60 挡对数刻度，也支持直接输入
//     最慢 3000ms/步，最快 1ms/步；界面与存档都只认这个毫秒值
// ============================================================================

static const int SPEED_MIN_MS = 1;
static const int SPEED_MAX_MS = 3000;
static const int SPEED_STEPS = 60;

static int clampDelayMs(int ms) {
    return std::min(SPEED_MAX_MS, std::max(SPEED_MIN_MS, ms));
}

// 挡位(1..60) → 毫秒：对数刻度，不然 1ms 那端会挤成一堆
static int speedStepToMs(int step) {
    if (step < 1) step = 1;
    if (step > SPEED_STEPS) step = SPEED_STEPS;
    const double t = (double)(step - 1) / (double)(SPEED_STEPS - 1);
    const double ms = (double)SPEED_MAX_MS *
                      std::pow((double)SPEED_MIN_MS / (double)SPEED_MAX_MS, t);
    return clampDelayMs((int)std::lround(ms));
}

// 毫秒 → 挡位（输入框改完用来同步滑块位置）
static int speedMsToStep(int ms) {
    ms = clampDelayMs(ms);
    const double t = std::log((double)ms / SPEED_MAX_MS) /
                     std::log((double)SPEED_MIN_MS / SPEED_MAX_MS);
    return std::min(SPEED_STEPS, std::max(1, (int)std::lround(t * (SPEED_STEPS - 1)) + 1));
}

// 「1.50s」/「250ms」这种目标速度文本
static void formatDelay(int ms, wchar_t* buf, size_t n) {
    if (ms >= 1000) std::swprintf(buf, n, L"%.2fs", ms / 1000.0);
    else std::swprintf(buf, n, L"%dms", ms);
}

// ============================================================================
//  四、存档（纯文本格式，方便自己看/改）
// ============================================================================

struct SaveData {
    int size = 4;
    int target = 2048;
    int score = 0;
    int moves = 0;
    int level = 1;          // AI 深度档
    int spawn = 0;          // 生成方块模式
    int autosave = 1;       // 启动时自动读取存档
    int delay = 250;        // AI 落子间隔（毫秒）
    int phase = 1;          // 1 进行中 / 2 已结束
    Grid grid{};
    std::vector<Snapshot> hist;
};

static std::wstring saveFilePath() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    dir += L"\\Game2048";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\save.dat";
}

// 最多只存 200 步历史，避免存档无限膨胀
static const size_t SAVE_HIST_MAX = 200;

static bool writeSave(const SaveData& d, const std::wstring& path) {
    FILE* fp = _wfopen(path.c_str(), L"w");
    if (!fp) return false;

    std::fprintf(fp, "v1\n");
    std::fprintf(fp, "size %d\ntarget %d\nscore %d\nmoves %d\nlevel %d\nspawn %d\nautosave %d\ndelay %d\nphase %d\n",
                 d.size, d.target, d.score, d.moves, d.level, d.spawn, d.autosave, d.delay, d.phase);

    std::fprintf(fp, "grid");
    for (int i = 0; i < d.size * d.size; ++i) std::fprintf(fp, " %d", d.grid.v[i]);
    std::fprintf(fp, "\n");

    size_t start = 0;
    if (d.hist.size() > SAVE_HIST_MAX) start = d.hist.size() - SAVE_HIST_MAX;
    std::fprintf(fp, "hist %d\n", (int)(d.hist.size() - start));
    for (size_t k = start; k < d.hist.size(); ++k) {
        const Snapshot& s = d.hist[k];
        std::fprintf(fp, "h %d %d", s.score, s.moves);
        for (int i = 0; i < d.size * d.size; ++i) std::fprintf(fp, " %d", s.grid.v[i]);
        std::fprintf(fp, "\n");
    }
    std::fclose(fp);
    return true;
}

static bool readSave(SaveData& d, const std::wstring& path) {
    FILE* fp = _wfopen(path.c_str(), L"r");
    if (!fp) return false;

    char tag[32];
    if (std::fscanf(fp, "%31s", tag) != 1 || std::strcmp(tag, "v1") != 0) {
        std::fclose(fp);
        return false;
    }

    while (std::fscanf(fp, "%31s", tag) == 1) {
        if (std::strcmp(tag, "size") == 0) {
            int n = 0;
            std::fscanf(fp, "%d", &n);
            if (n < 3 || n > MAXN) { std::fclose(fp); return false; }
            d.size = n;
            gridClear(d.grid, n);
        } else if (std::strcmp(tag, "target") == 0) std::fscanf(fp, "%d", &d.target);
        else if (std::strcmp(tag, "score") == 0) std::fscanf(fp, "%d", &d.score);
        else if (std::strcmp(tag, "moves") == 0) std::fscanf(fp, "%d", &d.moves);
        else if (std::strcmp(tag, "level") == 0) std::fscanf(fp, "%d", &d.level);
        else if (std::strcmp(tag, "spawn") == 0) std::fscanf(fp, "%d", &d.spawn);
        else if (std::strcmp(tag, "autosave") == 0) std::fscanf(fp, "%d", &d.autosave);
        else if (std::strcmp(tag, "delay") == 0) std::fscanf(fp, "%d", &d.delay);
        else if (std::strcmp(tag, "phase") == 0) std::fscanf(fp, "%d", &d.phase);
        else if (std::strcmp(tag, "grid") == 0) {
            for (int i = 0; i < d.size * d.size; ++i) {
                if (std::fscanf(fp, "%d", &d.grid.v[i]) != 1) { std::fclose(fp); return false; }
            }
        } else if (std::strcmp(tag, "hist") == 0) {
            int c = 0;
            std::fscanf(fp, "%d", &c);
            d.hist.clear();
            for (int k = 0; k < c; ++k) {
                Snapshot s;
                if (std::fscanf(fp, "%31s", tag) != 1 || std::strcmp(tag, "h") != 0) break;
                if (std::fscanf(fp, "%d %d", &s.score, &s.moves) != 2) break;
                gridClear(s.grid, d.size);
                bool ok = true;
                for (int i = 0; i < d.size * d.size; ++i) {
                    if (std::fscanf(fp, "%d", &s.grid.v[i]) != 1) { ok = false; break; }
                }
                if (!ok) break;
                d.hist.push_back(s);
            }
        } else {
            int ch;                                    // 未识别的内容，跳到行尾
            while ((ch = std::fgetc(fp)) != EOF && ch != '\n') {}
        }
    }
    std::fclose(fp);
    if (d.level < 0 || d.level >= AI_LEVEL_COUNT) d.level = 1;
    if (d.spawn < 0 || d.spawn > SPAWN_ONLY4) d.spawn = SPAWN_RANDOM;
    d.delay = clampDelayMs(d.delay);
    return true;
}

// ============================================================================
//  四、图形界面（Win32 GDI 全自绘，零外部依赖，无闪烁双缓冲）
// ============================================================================

#ifndef CONSOLE_BUILD

// ---- 配色（浅色主题） ----
static const COLORREF CLR_BG = RGB(0xfa, 0xf8, 0xef);
static const COLORREF CLR_BOARD = RGB(0xbb, 0xad, 0xa0);
static const COLORREF CLR_EMPTY = RGB(0xcd, 0xc1, 0xb4);
static const COLORREF CLR_DARKTXT = RGB(0x77, 0x6e, 0x65);
static const COLORREF CLR_LIGHTTXT = RGB(0xf9, 0xf6, 0xf2);
static const COLORREF CLR_ACCENT = RGB(0x8f, 0x7a, 0x66);
static const COLORREF CLR_STONE = RGB(0xa3, 0x94, 0x87);
static const COLORREF CLR_BTN = RGB(0xd6, 0xcd, 0xc4);
static const COLORREF CLR_BOX = RGB(0xee, 0xe4, 0xda);
static const COLORREF CLR_DISABLED = RGB(0xe8, 0xe2, 0xda);
static const COLORREF CLR_BORDER = RGB(0xe0, 0xd8, 0xce);

// 配置窗口 / 速度输入框共用的字体与画刷
static HBRUSH g_setBg = nullptr;
static HFONT g_setFont = nullptr, g_setFontB = nullptr;

// 每个可能的方块数值都有独立配色：2~2048 走经典暖色，4096 起转入紫→蓝→青→绿，
// 理论上 4×4 能到 131072、5×5 更高，这里一直排到 4194304，再大走深灰兜底。
static COLORREF tileColor(int v) {
    switch (v) {
        case 2: return RGB(0xee, 0xe4, 0xda);
        case 4: return RGB(0xed, 0xe0, 0xc8);
        case 8: return RGB(0xf2, 0xb1, 0x79);
        case 16: return RGB(0xf5, 0x95, 0x63);
        case 32: return RGB(0xf6, 0x7c, 0x5f);
        case 64: return RGB(0xf6, 0x5e, 0x3b);
        case 128: return RGB(0xed, 0xcf, 0x72);
        case 256: return RGB(0xed, 0xcc, 0x61);
        case 512: return RGB(0xed, 0xc8, 0x50);
        case 1024: return RGB(0xed, 0xc5, 0x3f);
        case 2048: return RGB(0xed, 0xc2, 0x2e);
        case 4096: return RGB(0x8f, 0x7a, 0xe6);    // 紫罗兰
        case 8192: return RGB(0x5b, 0x7f, 0xe0);    // 蓝
        case 16384: return RGB(0x2e, 0x9f, 0xd0);   // 天蓝
        case 32768: return RGB(0x1f, 0xa8, 0xa0);   // 青绿
        case 65536: return RGB(0x37, 0xa8, 0x4a);   // 绿
        case 131072: return RGB(0x7c, 0xb3, 0x42);  // 黄绿
        case 262144: return RGB(0xa1, 0x8a, 0x2e);  // 橄榄
        case 524288: return RGB(0xb0, 0x6a, 0x2e);  // 棕橙
        case 1048576: return RGB(0x9d, 0x4a, 0x3a); // 砖红
        case 2097152: return RGB(0x7a, 0x3a, 0x5a); // 酒红
        case 4194304: return RGB(0x4a, 0x4a, 0x5a); // 石墨
        default: return RGB(0x33, 0x33, 0x3d);      // 更大：深灰兜底
    }
}

// ---- 窗口尺寸 / 布局常量 ----
#define WM_APP_AI_DONE (WM_APP + 1)   // 工作线程算完 AI 后通知界面线程
static const int CLIENT_W = 528;
static const int CLIENT_H = 800;
static const int MARGIN = 14;
static const int BOARD_PX = 500;      // 棋盘正方形边长
static const int CELL_GAP = 10;       // 格子间距

// ---- 绘图小工具 ----
// 超采样抗锯齿：所有图形先画到 UI_SS 倍大的离屏位图，最后按比例缩小贴到窗口，
// 这样圆角、圆点、大号数字的锯齿会被「多像素平均」抹平（纯 GDI 也能有 AA）。
static const int UI_SS = 2;
static inline int S(int v) { return v * UI_SS; }
static inline RECT SR(const RECT& r) {
    return RECT{r.left * UI_SS, r.top * UI_SS, r.right * UI_SS, r.bottom * UI_SS};
}

static void fillRoundRect(HDC dc, const RECT& rl, int rad, COLORREF fill, COLORREF border) {
    RECT r = SR(rl);
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, UI_SS, border);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2 * UI_SS, rad * 2 * UI_SS);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void fillRect2(HDC dc, const RECT& rl, COLORREF fill) {
    RECT r = SR(rl);
    HBRUSH br = CreateSolidBrush(fill);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

// 画一个单选框圆点（选中 = 实心强调色）
static void drawRadioDot(HDC dc, const RECT& dotl, bool on) {
    RECT dot = SR(dotl);
    HBRUSH br = CreateSolidBrush(on ? CLR_ACCENT : CLR_BG);
    HPEN pen = CreatePen(PS_SOLID, 2 * UI_SS, on ? CLR_ACCENT : CLR_STONE);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

// 文字不参与超采样：形状画进 2× 位图缩小获得抗锯齿，文字统一记在队列里，
// 等形状缩小贴图完成后再用 1× 字号画在最上层（ClearType 高清，不会被缩糊）。
struct TextItem {
    RECT rc;
    std::wstring text;
    HFONT font;
    COLORREF color;
    UINT flags;
};
static std::vector<TextItem> g_texts;

static void drawTextIn(HDC dc, const RECT& rl, const wchar_t* s, HFONT f, COLORREF color, UINT flags) {
    (void)dc;
    TextItem t;
    t.rc = rl;
    t.text = s;
    t.font = f;
    t.color = color;
    t.flags = flags;
    g_texts.push_back(std::move(t));
}

// 贴图完成后调用：用 1× 字号把文字画到目标 DC 上
static void flushTexts(HDC dc) {
    for (const TextItem& t : g_texts) {
        HGDIOBJ of = SelectObject(dc, t.font);
        SetTextColor(dc, t.color);
        SetBkMode(dc, TRANSPARENT);
        RECT rr = t.rc;
        DrawTextW(dc, t.text.c_str(), -1, &rr, t.flags);
        SelectObject(dc, of);
    }
    g_texts.clear();
}

static bool inRect(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ---- 应用状态 ----
struct AppState {
    HWND hwnd = nullptr;
    HDC memDC = nullptr;
    HBITMAP memBmp = nullptr;
    int memW = 0, memH = 0;

    int selectedSize = 4;         // 开始前选中的棋盘大小（单选框）
    int aiLevel = 1;              // AI 深度档：0 快 / 1 标准 / 2 深 / 3 满负荷
    int phase = PHASE_SETUP;      // PHASE_SETUP / PHASE_RUNNING / PHASE_OVER
    Game game;                    // 当前这一局
    bool aiOn = false;
    int delayMs = 250;            // AI 落子间隔（毫秒），1~3000，滑块与输入框都改它
    HWND speedEdit = nullptr;     // 点「目标速度」后弹出的毫秒输入框
    bool draggingSlider = false;
    bool winNotified = false;
    std::mt19937 rng;
    std::wstring status;

    // ---- AI 异步搜索（放工作线程，最高档算几秒也不会卡住界面） ----
    std::thread worker;
    std::atomic<bool> aiBusy{false};
    std::atomic<bool> aiCancel{false};
    int pendingDir = -1;
    double pendingMs = 0.0;
    int pendingDepth = 0;
    double liveMs = 0.0;          // 实时速度（指数平滑，毫秒/步）
    bool hasLiveMs = false;
    int liveDepth = 0;
    std::chrono::steady_clock::time_point lastMoveTick;

    // ---- 配置窗口 / 存档 ----
    HWND settingsHwnd = nullptr;
    bool autosave = true;
    std::wstring saveNote;                                  // 存档状态提示
    std::chrono::steady_clock::time_point lastSaveTick;

    HFONT fTitle = nullptr, fSub = nullptr, fNum = nullptr, fUi = nullptr,
          fSmall = nullptr, fOver = nullptr, fOverSub = nullptr, fLogo = nullptr;
    HFONT tileFont[3][6] = {{nullptr}};   // [边长索引 0..2][数值位数档 0..5]

    RECT rcTitle{}, rcSub{}, rcSub2{}, rcBoard{}, rcStat[4]{}, rcMainBtn{}, rcSettingsBtn{},
         rcUndoBtn{}, rcAiBtn{}, rcSpeedLabel{}, rcSlider{}, rcSpeedText{}, rcStatus{};

    int displaySize() const { return (phase == PHASE_SETUP) ? selectedSize : game.n; }
};

static int aiDelayMs(const AppState& app);      // 定义在后面（速度那一节）

// ---- 前置声明（这些函数在文件后面才定义） ----
static void settingsCreate(AppState& app);
static void settingsSyncFromApp(AppState& app);
static bool appSaveGame(AppState& app, bool manual);
static bool appLoadGame(AppState& app, bool manual);
static void maybeAutosave(AppState& app);

static HFONT makeFont(int pixelHeight, bool bold) {
    // 文字按 1× 渲染（不参与超采样），这样小字号也保持 ClearType 清晰
    return CreateFontW(-pixelHeight, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

static void createFonts(AppState& app) {
    // 主界面下方的字号按「低分辨率也要看得清」放大过（比最初的版本大 30%~35%）
    app.fTitle = makeFont(34, true);
    app.fLogo = makeFont(24, true);
    app.fSub = makeFont(13, false);
    app.fNum = makeFont(20, true);
    app.fUi = makeFont(16, false);
    app.fSmall = makeFont(13, false);
    app.fOver = makeFont(22, true);
    app.fOverSub = makeFont(13, false);

    const double scales[6] = {0.44, 0.37, 0.30, 0.24, 0.20, 0.16};
    for (int i = 0; i < 3; ++i) {
        int n = 3 + i;
        double cell = (double)(BOARD_PX - CELL_GAP * (n + 1)) / n;
        for (int c = 0; c < 6; ++c) {
            app.tileFont[i][c] = makeFont((int)(cell * scales[c]), true);
        }
    }
}

static void destroyFonts(AppState& app) {
    HFONT* list[] = {&app.fTitle, &app.fSub, &app.fNum, &app.fUi, &app.fSmall,
                     &app.fOver, &app.fOverSub, &app.fLogo};
    for (HFONT* f : list) {
        if (*f) { DeleteObject(*f); *f = nullptr; }
    }
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 6; ++c)
            if (app.tileFont[i][c]) { DeleteObject(app.tileFont[i][c]); app.tileFont[i][c] = nullptr; }
}

static void layout(AppState& app) {
    app.rcTitle = {14, 12, 104, 81};                 // 左上角「2048」文字标题
    app.rcSub = {108, 28, 330, 60};
    app.rcSub2 = {330, 28, CLIENT_W - MARGIN, 60};

    int sy = 94, sh = 56, gap = 6;                   // 数据框（分数/步数/最大方块/AI速度）
    int bw = (CLIENT_W - 2 * MARGIN - 3 * gap) / 4;
    for (int i = 0; i < 4; ++i) {
        int x = MARGIN + i * (bw + gap);
        app.rcStat[i] = {x, sy, x + bw, sy + sh};
    }
    int boardTop = sy + sh + 8;
    app.rcBoard = {MARGIN, boardTop, MARGIN + BOARD_PX, boardTop + BOARD_PX};

    // 按钮排成一行：撤回 / AI 开关 / 配置设置 / 开始重新开始（原来撤回和 AI 单独占一行，左侧留了空白）
    int by = app.rcBoard.bottom + 12;
    app.rcUndoBtn = {MARGIN, by, MARGIN + 100, by + 40};
    app.rcAiBtn = {MARGIN + 108, by, MARGIN + 208, by + 40};
    app.rcSettingsBtn = {MARGIN + 228, by, MARGIN + 352, by + 40};
    app.rcMainBtn = {MARGIN + 362, by, MARGIN + 500, by + 40};

    // AI 速度标签 + 拉长铺满的滑块
    int cy = by + 48;
    app.rcSpeedLabel = {MARGIN, cy, MARGIN + 92, cy + 40};
    app.rcSlider = {MARGIN + 96, cy + 8, CLIENT_W - MARGIN - 108, cy + 32};
    app.rcSpeedText = {app.rcSlider.right + 8, cy, CLIENT_W - MARGIN, cy + 40};
    app.rcStatus = {MARGIN, cy + 48, CLIENT_W - MARGIN, cy + 76};
}

static void setStatus(AppState& app, const wchar_t* text) {
    if (app.status != text) {
        app.status = text;
        InvalidateRect(app.hwnd, &app.rcStatus, FALSE);
    }
}

static void refresh(AppState& app) {
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// ---- 棋盘绘制 ----
static void paintBoard(AppState& app, HDC dc) {
    const int n = app.displaySize();
    fillRoundRect(dc, app.rcBoard, 8, CLR_BOARD, CLR_BOARD);

    const double gap = CELL_GAP;
    const double cell = (double)(BOARD_PX - gap * (n + 1)) / n;
    const int nIdx = n - 3;

    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            const double x0 = app.rcBoard.left + gap + c * (cell + gap);
            const double y0 = app.rcBoard.top + gap + r * (cell + gap);
            RECT rc = {(LONG)x0, (LONG)y0, (LONG)(x0 + cell), (LONG)(y0 + cell)};
            int v = app.game.grid.v[GIDX(n, r, c)];
            COLORREF fill = v ? tileColor(v) : CLR_EMPTY;
            fillRoundRect(dc, rc, (int)(cell * 0.09) + 2, fill, fill);
            if (!v) continue;

            int cls;
            if (v < 10) cls = 0;
            else if (v < 100) cls = 1;
            else if (v < 1000) cls = 2;
            else if (v < 10000) cls = 3;
            else if (v < 100000) cls = 4;
            else cls = 5;

            wchar_t buf[24];
            std::swprintf(buf, 24, L"%d", v);
            drawTextIn(dc, rc, buf, app.tileFont[nIdx][cls],
                       (v <= 4) ? CLR_DARKTXT : CLR_LIGHTTXT,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
}

static void paintOverlayCard(AppState& app, HDC dc, const wchar_t* title, const wchar_t* sub) {
    int w = 380, h = 118;
    RECT rc = {app.rcBoard.left + (BOARD_PX - w) / 2, app.rcBoard.top + (BOARD_PX - h) / 2 - 10,
               app.rcBoard.left + (BOARD_PX - w) / 2 + w, app.rcBoard.top + (BOARD_PX - h) / 2 - 10 + h};
    fillRoundRect(dc, rc, 10, CLR_BG, CLR_BORDER);
    RECT rt = {rc.left, rc.top + 22, rc.right, rc.top + 58};
    RECT rs = {rc.left, rc.top + 62, rc.right, rc.top + 90};
    drawTextIn(dc, rt, title, app.fOver, CLR_DARKTXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    drawTextIn(dc, rs, sub, app.fOverSub, CLR_ACCENT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paintAll(AppState& app, HDC dc) {
    g_texts.clear();
    RECT client = {0, 0, CLIENT_W, CLIENT_H};
    fillRect2(dc, client, CLR_BG);

    // ---- 左上角标题 ----
    drawTextIn(dc, app.rcTitle, L"2048", app.fTitle, CLR_DARKTXT,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    drawTextIn(dc, app.rcSub, L"方向键 / WASD 操作",
               app.fSub, CLR_STONE, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // 右上角一行当前配置摘要
    {
        const wchar_t* spawnName = (g_spawnMode == SPAWN_ONLY2) ? L"全2"
                                   : (g_spawnMode == SPAWN_ONLY4 ? L"全4" : L"随机");
        wchar_t summary[128];
        std::swprintf(summary, 128, L"%d×%d · %ls · %ls", app.displaySize(), app.displaySize(),
                      AI_LEVELS[app.aiLevel].name, spawnName);
        drawTextIn(dc, app.rcSub2, summary, app.fSub, CLR_STONE,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // ---- 数据栏（分数 / 步数 / 最大方块 / 实时 AI 速度） ----
    wchar_t speedVal[24];
    wchar_t speedCap[32];
    if (app.aiOn && app.hasLiveMs) {
        if (app.liveMs >= 1000.0) std::swprintf(speedVal, 24, L"%.2fs", app.liveMs / 1000.0);
        else std::swprintf(speedVal, 24, L"%.1fms", app.liveMs);
        std::swprintf(speedCap, 32, L"AI速度 %d层", app.liveDepth);
    } else if (app.aiOn) {
        std::swprintf(speedVal, 24, L"…");
        std::swprintf(speedCap, 32, L"AI 思考中");
    } else {
        std::swprintf(speedVal, 24, L"—");
        std::swprintf(speedCap, 32, L"AI 速度");
    }

    const wchar_t* caps[4] = {L"分数", L"步数", L"最大方块", speedCap};
    wchar_t vals[4][24];
    if (app.phase == PHASE_SETUP) {
        std::swprintf(vals[0], 24, L"0");
        std::swprintf(vals[1], 24, L"0");
        std::swprintf(vals[2], 24, L"0");
    } else {
        std::swprintf(vals[0], 24, L"%d", app.game.score);
        std::swprintf(vals[1], 24, L"%d", app.game.moves);
        std::swprintf(vals[2], 24, L"%d", gridMax(app.game.grid, app.game.n));
    }
    std::swprintf(vals[3], 24, L"%ls", speedVal);

    for (int i = 0; i < 4; ++i) {
        fillRoundRect(dc, app.rcStat[i], 8, CLR_BOX, CLR_BOX);
        RECT rv = app.rcStat[i];
        rv.top += 6; rv.bottom = rv.top + 26;
        RECT rc2 = app.rcStat[i];
        rc2.top += 32; rc2.bottom -= 2;
        drawTextIn(dc, rv, vals[i], app.fNum, CLR_DARKTXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        drawTextIn(dc, rc2, caps[i], app.fSmall, CLR_STONE, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    paintBoard(app, dc);

    if (app.phase == PHASE_SETUP) {
        paintOverlayCard(app, dc, L"准备开始 2048", L"棋盘大小 / AI 深度在「配置 / 设置」里调");
    } else if (app.phase == PHASE_OVER) {
        paintOverlayCard(app, dc, L"无路可走了", L"点「撤回」可回退一步续命");
    }

    // ---- 主按钮（开始游戏 / 重新开始）+ 设置按钮 ----
    const bool running = (app.phase == PHASE_RUNNING);
    const wchar_t* mainText = (app.phase == PHASE_SETUP) ? L"开始游戏" : L"重新开始";
    fillRoundRect(dc, app.rcMainBtn, 6, CLR_ACCENT, CLR_ACCENT);
    drawTextIn(dc, app.rcMainBtn, mainText, app.fUi, CLR_LIGHTTXT,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    fillRoundRect(dc, app.rcSettingsBtn, 6, CLR_BTN, CLR_BTN);
    drawTextIn(dc, app.rcSettingsBtn, L"配置 / 设置", app.fUi, CLR_DARKTXT,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // ---- 棋盘大小 / AI 深度 / 生成方块 / 存档全部在「配置 / 设置」窗口里 ----

    // ---- 撤回按钮 ----
    const bool canUndo = app.game.canUndo();
    fillRoundRect(dc, app.rcUndoBtn, 6, canUndo ? CLR_BTN : CLR_DISABLED,
                  canUndo ? CLR_BTN : CLR_DISABLED);
    drawTextIn(dc, app.rcUndoBtn, L"撤回", app.fUi, canUndo ? CLR_DARKTXT : RGB(0xb3, 0xaa, 0xa0),
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // ---- AI 开关 ----
    COLORREF aiFill = app.aiOn ? CLR_ACCENT : (running ? CLR_BTN : CLR_DISABLED);
    COLORREF aiText = app.aiOn ? CLR_LIGHTTXT : (running ? CLR_DARKTXT : RGB(0xb3, 0xaa, 0xa0));
    fillRoundRect(dc, app.rcAiBtn, 6, aiFill, aiFill);
    drawTextIn(dc, app.rcAiBtn, app.aiOn ? L"AI：开" : L"AI：关", app.fUi, aiText,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // ---- AI 速度滑块 ----
    drawTextIn(dc, app.rcSpeedLabel, L"AI 速度", app.fUi, CLR_DARKTXT,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int trackL = app.rcSlider.left + 9, trackR = app.rcSlider.right - 9;
    int trackY = (app.rcSlider.top + app.rcSlider.bottom) / 2;
    RECT rcTrack = {trackL, trackY - 3, trackR, trackY + 3};
    fillRoundRect(dc, rcTrack, 3, CLR_BTN, CLR_BTN);
    const int step = speedMsToStep(app.delayMs);
    const double t = (double)(step - 1) / (double)(SPEED_STEPS - 1);
    int knobX = trackL + (int)std::lround(t * (trackR - trackL));
    RECT rcFill = {trackL, trackY - 3, knobX, trackY + 3};
    fillRoundRect(dc, rcFill, 3, CLR_ACCENT, CLR_ACCENT);
    HBRUSH kb = CreateSolidBrush(CLR_ACCENT);
    HBRUSH kob = (HBRUSH)SelectObject(dc, kb);
    HPEN kp = CreatePen(PS_SOLID, 2 * UI_SS, CLR_LIGHTTXT);
    HPEN kop = (HPEN)SelectObject(dc, kp);
    Ellipse(dc, S(knobX - 9), S(trackY - 9), S(knobX + 9), S(trackY + 9));
    SelectObject(dc, kob);
    SelectObject(dc, kop);
    DeleteObject(kb);
    DeleteObject(kp);

    // 右侧显示「目标速度」，点它可以输入精确毫秒
    {
        RECT box = app.rcSpeedText;
        box.top += 4; box.bottom -= 4;
        fillRoundRect(dc, box, 6, CLR_BTN, CLR_BTN);
        wchar_t sp[24];
        formatDelay(aiDelayMs(app), sp, 24);
        drawTextIn(dc, box, sp, app.fSmall, CLR_DARKTXT,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // ---- 状态栏 ----
    drawTextIn(dc, app.rcStatus, app.status.c_str(), app.fSmall, CLR_ACCENT,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---- 离屏位图（按超采样倍数放大，绘制完再缩小贴到窗口） ----
static void ensureMemDC(AppState& app) {
    const int bw = S(CLIENT_W), bh = S(CLIENT_H);
    if (app.memDC && app.memW == bw && app.memH == bh) return;
    if (app.memDC) { DeleteDC(app.memDC); app.memDC = nullptr; }
    if (app.memBmp) { DeleteObject(app.memBmp); app.memBmp = nullptr; }
    HDC screen = GetDC(app.hwnd);
    app.memDC = CreateCompatibleDC(screen);
    app.memBmp = CreateCompatibleBitmap(screen, bw, bh);
    SelectObject(app.memDC, app.memBmp);
    app.memW = bw;
    app.memH = bh;
    ReleaseDC(app.hwnd, screen);
}

// 把离屏位图缩小贴到目标 DC（HALFTONE 会做多像素平均 → 抗锯齿）
static void blitTo(HDC dst, AppState& app) {
    SetStretchBltMode(dst, HALFTONE);
    SetBrushOrgEx(dst, 0, 0, nullptr);
    StretchBlt(dst, 0, 0, CLIENT_W, CLIENT_H, app.memDC, 0, 0, S(CLIENT_W), S(CLIENT_H), SRCCOPY);
}

// 当前落子间隔（毫秒）——滑块 60 挡对数刻度，也支持直接输入
static int aiDelayMs(const AppState& app) { return clampDelayMs(app.delayMs); }

// AI 速度只控制「落子间隔」，搜索深度由 AI 深度档决定（见 applyAiLevel）
static void cancelAiSearch(AppState& app);
static void startAiSearch(AppState& app);
static void finishAiSearch(AppState& app);

// ---- 玩家操作 ----
static void startGame(AppState& app) {
    cancelAiSearch(app);
    app.game.reset(app.selectedSize);
    spawnTile(app.game.grid, app.game.n, app.rng);
    spawnTile(app.game.grid, app.game.n, app.rng);
    app.phase = PHASE_RUNNING;
    app.winNotified = false;
    app.aiOn = false;

    wchar_t buf[220];
    if (app.game.n == 3) {
        std::swprintf(buf, 220,
                      L"3×3 开局，目标 %d（只有 9 格，理论极限 1024、2048 无法出现）；方向键 / WASD 操作",
                      app.game.target);
    } else {
        std::swprintf(buf, 220, L"%d×%d 开局，目标 %d；方向键 / WASD 操作",
                      app.game.n, app.game.n, app.game.target);
    }
    setStatus(app, buf);
    refresh(app);
}

static void checkAfterMove(AppState& app) {
    Game& g = app.game;
    if (g.reached && !app.winNotified) {
        app.winNotified = true;
        wchar_t buf[200];
        std::swprintf(buf, 200, L"达成 %d！已锁定胜局，可以继续冲更大的数字", g.target);
        setStatus(app, buf);
    }
    if (g.over) {
        app.phase = PHASE_OVER;
        cancelAiSearch(app);
        app.aiOn = false;
        wchar_t buf[200];
        std::swprintf(buf, 200, L"无路可走：%d 分 / %d 步（点「撤回」可回退一步续命）", g.score, g.moves);
        setStatus(app, buf);
    }
    maybeAutosave(app);
    refresh(app);
}

static void humanMove(AppState& app, int dir) {
    if (app.phase != PHASE_RUNNING) return;
    if (app.aiOn) {
        setStatus(app, L"AI 正在托管，先点「AI：关」才能手动操作");
        return;
    }
    if (app.game.doMove(dir, app.rng)) checkAfterMove(app);
}

static void doUndo(AppState& app) {
    cancelAiSearch(app);
    if (!app.game.canUndo()) {
        setStatus(app, L"当前没有可以撤回的步骤");
        return;
    }
    app.game.undo();
    if (app.phase == PHASE_OVER) app.phase = PHASE_RUNNING;
    setStatus(app, L"已撤回一步（分数与步数一并回退）");
    refresh(app);
}

static void toggleAI(AppState& app) {
    if (app.phase != PHASE_RUNNING) {
        setStatus(app, L"先点「开始游戏」，AI 才能上场");
        return;
    }
    if (app.aiOn) cancelAiSearch(app);         // 关掉时立刻终止正在进行的搜索
    app.aiOn = !app.aiOn;
    app.hasLiveMs = false;
    if (app.aiOn) {
        const AiLevel& lv = AI_LEVELS[app.aiLevel];
        const bool mt = (lv.depth == 0 || lv.depth >= 4);      // 自适应或 ≥4 层才走多线程
        wchar_t buf[200];
        std::swprintf(buf, 200, L"AI 已开启：深度「%ls」%ls，速度滑块只控制落子快慢",
                      lv.name, mt ? L"（多线程并行）" : L"（单线程）");
        setStatus(app, buf);
    } else {
        setStatus(app, L"AI 已关闭，交给你操作");
    }
    refresh(app);
}

// ---- AI 深度档 → 全局搜索参数（线程数 0 表示用满本机所有核心） ----
static void applyAiLevel(AppState& app) {
    const AiLevel& lv = AI_LEVELS[app.aiLevel];
    g_fixedDepth = lv.depth;
    g_probCutoff = lv.cutoff;
    g_thinkBudgetMs = lv.budgetMs;
    g_threads = lv.threads;          // 0 = 用满本机所有核心（靠线程池摊薄开销）
}

// 取消并等待正在进行的搜索（工作线程每 512 个节点检查一次取消标志，通常几十微秒就返回）
static void cancelAiSearch(AppState& app) {
    app.aiCancel.store(true);
    if (app.worker.joinable()) app.worker.join();
    app.aiBusy.store(false);
    app.pendingDir = -1;
}

// 把搜索丢到工作线程：最高档要算几秒，放在界面线程会卡死窗口
static void startAiSearch(AppState& app) {
    cancelAiSearch(app);
    applyAiLevel(app);

    app.aiCancel.store(false);
    app.aiBusy.store(true);
    app.pendingDir = -1;

    const Grid snapshot = app.game.grid;
    const int n = app.game.n;
    const unsigned seed = (unsigned)app.rng();
    AppState* self = &app;

    app.worker = std::thread([self, snapshot, n, seed]() {
        std::mt19937 rng(seed);
        AiChoice c = chooseMove(snapshot, n, rng, &self->aiCancel);
        self->pendingDir = c.dir;
        self->pendingMs = c.ms;
        self->pendingDepth = c.depth;
        // 注意：不清 aiBusy，交给界面线程在 finishAiSearch 里清，
        // 否则消息还没被处理，主循环就会误以为空闲而再起一次搜索
        PostMessageW(self->hwnd, WM_APP_AI_DONE, 0, 0);
    });
}

// 工作线程算完 → 在界面线程里落子
static void finishAiSearch(AppState& app) {
    if (app.worker.joinable()) app.worker.join();
    app.aiBusy.store(false);
    if (!app.aiOn || app.phase != PHASE_RUNNING || app.aiCancel.load()) return;

    // 实时速度：指数平滑，避免数字乱跳
    app.liveMs = app.hasLiveMs ? (app.liveMs * 0.65 + app.pendingMs * 0.35) : app.pendingMs;
    app.hasLiveMs = true;
    app.liveDepth = app.pendingDepth;
    app.lastMoveTick = std::chrono::steady_clock::now();

    if (app.pendingDir >= 0) app.game.doMove(app.pendingDir, app.rng);
    checkAfterMove(app);
}

// ---- 目标速度输入框（点主界面右侧的「1500ms」弹出，输入 1~3000 毫秒） ----
static WNDPROC g_speedEditOldProc = nullptr;
static void commitSpeedEdit(AppState& app);
static void cancelSpeedEdit(AppState& app);

static LRESULT CALLBACK speedEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState* app = (AppState*)GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA);
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_RETURN) { if (app) commitSpeedEdit(*app); return 0; }
            if (wp == VK_ESCAPE) { if (app) cancelSpeedEdit(*app); return 0; }
            break;
        case WM_CHAR:
            if (wp == VK_RETURN || wp == 27) return 0;      // 不然会“叮”一声
            break;
        case WM_KILLFOCUS:                                  // 点到别处就当作确认
            if (app && app->speedEdit == hwnd) commitSpeedEdit(*app);
            return 0;
        case WM_NCDESTROY:
            if (g_speedEditOldProc) {
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)g_speedEditOldProc);
            }
            break;
        default:
            break;
    }
    return g_speedEditOldProc ? CallWindowProcW(g_speedEditOldProc, hwnd, msg, wp, lp)
                              : DefWindowProcW(hwnd, msg, wp, lp);
}

static void openSpeedEdit(AppState& app) {
    if (app.speedEdit) {
        SetFocus(app.speedEdit);
        return;
    }
    if (!g_setBg) {
        g_setBg = CreateSolidBrush(CLR_BG);
        g_setFont = makeFont(13, false);
        g_setFontB = makeFont(14, true);
    }
    RECT r = app.rcSpeedText;
    r.top += 5;
    r.bottom -= 5;
    wchar_t buf[16];
    std::swprintf(buf, 16, L"%d", aiDelayMs(app));
    app.speedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", buf,
                                    WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                                    r.left + 2, r.top + 2, (r.right - r.left) - 4, (r.bottom - r.top) - 4,
                                    app.hwnd, (HMENU)(INT_PTR)9001,
                                    (HINSTANCE)GetWindowLongPtrW(app.hwnd, GWLP_HINSTANCE), nullptr);
    if (!app.speedEdit) return;
    SendMessageW(app.speedEdit, WM_SETFONT, (WPARAM)g_setFont, TRUE);
    SendMessageW(app.speedEdit, EM_SETSEL, 0, -1);
    g_speedEditOldProc = (WNDPROC)SetWindowLongPtrW(app.speedEdit, GWLP_WNDPROC, (LONG_PTR)speedEditProc);
    SetFocus(app.speedEdit);
    setStatus(app, L"输入 1~3000 毫秒后回车（Esc 取消）");
    refresh(app);
}

static void commitSpeedEdit(AppState& app) {
    HWND e = app.speedEdit;
    if (!e) return;
    app.speedEdit = nullptr;                    // 先置空，避免销毁消息里重入
    wchar_t buf[32] = {0};
    GetWindowTextW(e, buf, 31);
    const int typed = _wtoi(buf);
    const int ms = clampDelayMs(typed > 0 ? typed : app.delayMs);
    app.delayMs = ms;
    DestroyWindow(e);
    SetFocus(app.hwnd);

    wchar_t b2[96];
    if (ms >= SPEED_MAX_MS) std::swprintf(b2, 96, L"目标速度 = 最慢 %dms/步", ms);
    else if (ms <= SPEED_MIN_MS) std::swprintf(b2, 96, L"目标速度 = 最快 %dms/步", ms);
    else std::swprintf(b2, 96, L"目标速度 = %dms/步", ms);
    setStatus(app, b2);
    appSaveGame(app, false);        // 速度属于配置，改完立刻落盘
    refresh(app);
}

static void cancelSpeedEdit(AppState& app) {
    HWND e = app.speedEdit;
    if (!e) return;
    app.speedEdit = nullptr;
    DestroyWindow(e);
    SetFocus(app.hwnd);
    refresh(app);
}

static void sliderFromX(AppState& app, int x) {
    int trackL = app.rcSlider.left + 9;
    int trackR = app.rcSlider.right - 9;
    double t = (double)(x - trackL) / (double)(trackR - trackL);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    const int step = 1 + (int)std::lround(t * (SPEED_STEPS - 1));
    const int ms = speedStepToMs(step);
    if (ms != app.delayMs) {
        app.delayMs = ms;
        refresh(app);
    }
}

static void onLButtonDown(AppState& app, int x, int y) {
    if (inRect(app.rcMainBtn, x, y)) { startGame(app); return; }
    if (inRect(app.rcSettingsBtn, x, y)) { settingsCreate(app); return; }
    if (inRect(app.rcUndoBtn, x, y)) { doUndo(app); return; }
    if (inRect(app.rcAiBtn, x, y)) { toggleAI(app); return; }

    RECT box = app.rcSpeedText;
    box.left -= 6; box.right += 6; box.top -= 4; box.bottom += 4;
    if (inRect(box, x, y)) { openSpeedEdit(app); return; }   // 点目标速度 → 输入毫秒

    RECT hit = app.rcSlider;
    hit.left -= 8; hit.right += 8; hit.top -= 10; hit.bottom += 10;
    if (inRect(hit, x, y)) {
        app.draggingSlider = true;
        SetCapture(app.hwnd);
        sliderFromX(app, x);
    }
}

static void onMouseMove(AppState& app, int x, int y) {
    if (app.draggingSlider) sliderFromX(app, x);
}

static void onLButtonUp(AppState& app) {
    if (app.draggingSlider) {
        app.draggingSlider = false;
        ReleaseCapture();
        appSaveGame(app, false);    // 松手时存一次
    }
}

static void onKeyDown(AppState& app, int vk) {
    int dir = -1;
    switch (vk) {
        case VK_UP: case 'W': case VK_NUMPAD8: dir = DIR_UP; break;
        case VK_DOWN: case 'S': case VK_NUMPAD2: dir = DIR_DOWN; break;
        case VK_LEFT: case 'A': case VK_NUMPAD4: dir = DIR_LEFT; break;
        case VK_RIGHT: case 'D': case VK_NUMPAD6: dir = DIR_RIGHT; break;
        case VK_RETURN:
            if (app.phase != PHASE_RUNNING) startGame(app);
            return;
        default:
            return;
    }
    humanMove(app, dir);
}

// ============================================================================
//  配置窗口（独立的子窗口，用系统原生控件，风格与主界面保持一致）
// ============================================================================

enum {
    IDC_RB_SIZE3 = 100, IDC_RB_SIZE4, IDC_RB_SIZE5,
    IDC_RB_L0 = 110, IDC_RB_L1, IDC_RB_L2, IDC_RB_L3, IDC_RB_L4, IDC_RB_L5,
    IDC_CB_ONLY2 = 200, IDC_CB_ONLY4, IDC_CB_AUTOSAVE,
    IDC_BTN_SAVE = 300, IDC_BTN_LOAD, IDC_BTN_CLOSE,
    IDC_ST_SAVEINFO = 310, IDC_ST_HINT,
};

static const int SET_W = 420;
static const int SET_H = 560;

// ---- 存档读写（带界面提示的封装） ----
static bool appSaveGame(AppState& app, bool manual) {
    SaveData d;
    d.size = app.game.n;
    d.target = app.game.target;
    d.score = app.game.score;
    d.moves = app.game.moves;
    d.level = app.aiLevel;
    d.spawn = g_spawnMode;
    d.autosave = app.autosave ? 1 : 0;
    d.delay = clampDelayMs(app.delayMs);
    d.phase = (app.phase == PHASE_OVER) ? 2 : 1;
    d.grid = app.game.grid;
    d.hist = app.game.history;

    const bool ok = writeSave(d, saveFilePath());
    if (ok) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t buf[200];
        std::swprintf(buf, 200, L"上次存档 %02d:%02d:%02d\n%d 分 / %d 步 / %d×%d",
                      st.wHour, st.wMinute, st.wSecond, app.game.score, app.game.moves,
                      app.game.n, app.game.n);        app.saveNote = buf;
        app.lastSaveTick = std::chrono::steady_clock::now();
    }
    if (manual) {
        setStatus(app, ok ? L"已保存存档：%APPDATA%\\Game2048\\save.dat"
                          : L"存档写入失败，目录不可写");
    }
    return ok;
}

static bool appLoadGame(AppState& app, bool manual) {
    SaveData d;
    if (!readSave(d, saveFilePath())) {
        if (manual) setStatus(app, L"没有找到可读取的存档（开一局会自动存）");
        return false;
    }
    app.selectedSize = d.size;
    app.aiLevel = d.level;
    g_spawnMode = d.spawn;
    app.autosave = (d.autosave != 0);
    app.delayMs = clampDelayMs(d.delay);

    app.game.n = d.size;
    app.game.target = targetForSize(d.size);
    app.game.grid = d.grid;
    app.game.score = d.score;
    app.game.moves = d.moves;
    app.game.history = d.hist;
    app.game.over = isGameOver(app.game.grid, d.size);
    app.game.reached = gridMax(app.game.grid, d.size) >= app.game.target;

    app.phase = app.game.over ? PHASE_OVER : PHASE_RUNNING;
    app.winNotified = app.game.reached;
    app.aiOn = false;
    app.hasLiveMs = false;
    app.lastMoveTick = std::chrono::steady_clock::now();

    if (app.settingsHwnd) settingsSyncFromApp(app);
    if (manual) {
        wchar_t buf[200];
        std::swprintf(buf, 200, L"已读取存档：%d×%d，%d 分 / %d 步", d.size, d.size, d.score, d.moves);
        setStatus(app, buf);
    }
    refresh(app);
    return true;
}

// 自动存档：每 1.5 秒最多写一次，避免高频落盘
static void maybeAutosave(AppState& app) {
    if (!app.autosave) return;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - app.lastSaveTick)
                          .count();
    if (ms < 1500.0) return;
    appSaveGame(app, false);
}

static void settingsRefreshTexts(AppState& app) {
    if (!app.settingsHwnd) return;
    wchar_t buf[260];
    if (app.saveNote.empty()) {
        std::swprintf(buf, 260, L"还没有存档，开局后会自动保存");
    } else {
        std::swprintf(buf, 260, L"%ls", app.saveNote.c_str());
    }
    SetDlgItemTextW(app.settingsHwnd, IDC_ST_SAVEINFO, buf);
}

static void settingsSyncFromApp(AppState& app) {
    HWND h = app.settingsHwnd;
    if (!h) return;
    const int sizeIds[6] = {0, 0, 0, IDC_RB_SIZE3, IDC_RB_SIZE4, IDC_RB_SIZE5};
    CheckRadioButton(h, IDC_RB_SIZE3, IDC_RB_SIZE5, sizeIds[app.selectedSize]);
    CheckRadioButton(h, IDC_RB_L0, IDC_RB_L5, IDC_RB_L0 + app.aiLevel);
    CheckDlgButton(h, IDC_CB_ONLY2, (g_spawnMode == SPAWN_ONLY2) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_CB_ONLY4, (g_spawnMode == SPAWN_ONLY4) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_CB_AUTOSAVE, app.autosave ? BST_CHECKED : BST_UNCHECKED);
    settingsRefreshTexts(app);
}

static HWND settingsAdd(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                        int x, int y, int w, int hgt, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, hgt, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_setFont, TRUE);
    return c;
}

static void settingsCreate(AppState& app) {
    if (app.settingsHwnd) {
        SetForegroundWindow(app.settingsHwnd);
        return;
    }
    if (!g_setBg) {
        g_setBg = CreateSolidBrush(CLR_BG);
        g_setFont = makeFont(13, false);     // 与主界面说明文字同号
        g_setFontB = makeFont(14, true);
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT r = {0, 0, SET_W, SET_H};
    AdjustWindowRect(&r, style, FALSE);

    HWND h = CreateWindowExW(0, L"Game2048SettingsClass", L"配置 / 设置", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             app.hwnd, nullptr, (HINSTANCE)GetWindowLongPtrW(app.hwnd, GWLP_HINSTANCE), &app);
    if (!h) return;
    app.settingsHwnd = h;
    settingsSyncFromApp(app);
    ShowWindow(h, SW_SHOW);
}

// ---- 配置窗口过程 ----
static LRESULT CALLBACK settingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        AppState* a = (AppState*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)a);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    AppState* app = (AppState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE: {
            // 棋盘大小
            settingsAdd(hwnd, L"BUTTON", L"棋盘大小", BS_GROUPBOX, 16, 28, SET_W - 32, 66, 0);
            settingsAdd(hwnd, L"BUTTON", L"3×3", BS_AUTORADIOBUTTON | WS_GROUP, 34, 50, 80, 26, IDC_RB_SIZE3);
            settingsAdd(hwnd, L"BUTTON", L"4×4", BS_AUTORADIOBUTTON, 140, 50, 80, 26, IDC_RB_SIZE4);
            settingsAdd(hwnd, L"BUTTON", L"5×5", BS_AUTORADIOBUTTON, 246, 50, 80, 26, IDC_RB_SIZE5);

            // AI 深度（6 档，2 列 × 3 行）
            settingsAdd(hwnd, L"BUTTON", L"AI 深度（越深越强、也越慢）", BS_GROUPBOX, 16, 102, SET_W - 32, 118, 0);
            settingsAdd(hwnd, L"BUTTON", L"快·3层", BS_AUTORADIOBUTTON | WS_GROUP, 34, 124, 140, 26, IDC_RB_L0);
            settingsAdd(hwnd, L"BUTTON", L"标准·4层", BS_AUTORADIOBUTTON, 196, 124, 160, 26, IDC_RB_L1);
            settingsAdd(hwnd, L"BUTTON", L"深·5层", BS_AUTORADIOBUTTON, 34, 152, 140, 26, IDC_RB_L2);
            settingsAdd(hwnd, L"BUTTON", L"更深·6层", BS_AUTORADIOBUTTON, 196, 152, 160, 26, IDC_RB_L3);
            settingsAdd(hwnd, L"BUTTON", L"极限·7层", BS_AUTORADIOBUTTON, 34, 180, 140, 26, IDC_RB_L4);
            settingsAdd(hwnd, L"BUTTON", L"满负荷·自适应", BS_AUTORADIOBUTTON, 196, 180, 170, 26, IDC_RB_L5);

            // 生成方块（测试用）
            settingsAdd(hwnd, L"BUTTON", L"生成方块（测试用）", BS_GROUPBOX, 16, 242, SET_W - 32, 108, 0);
            settingsAdd(hwnd, L"BUTTON", L"全 2 模式（只生成 2）", BS_AUTOCHECKBOX, 34, 266, 300, 26, IDC_CB_ONLY2);
            settingsAdd(hwnd, L"BUTTON", L"全 4 模式（只生成 4）", BS_AUTOCHECKBOX, 34, 296, 300, 26, IDC_CB_ONLY4);
            settingsAdd(hwnd, L"STATIC", L"都不勾 = 正常随机（90% 出 2）", SS_LEFT,
                        34, 324, 340, 22, IDC_ST_HINT);

            // 存档
            settingsAdd(hwnd, L"BUTTON", L"存档", BS_GROUPBOX, 16, 358, SET_W - 32, 148, 0);
            settingsAdd(hwnd, L"STATIC", L"", SS_LEFT, 34, 380, SET_W - 68, 50, IDC_ST_SAVEINFO);
            settingsAdd(hwnd, L"BUTTON", L"保存存档", BS_PUSHBUTTON, 34, 434, 110, 30, IDC_BTN_SAVE);
            settingsAdd(hwnd, L"BUTTON", L"读取存档", BS_PUSHBUTTON, 156, 434, 110, 30, IDC_BTN_LOAD);
            settingsAdd(hwnd, L"BUTTON", L"启动时自动读取存档", BS_AUTOCHECKBOX, 34, 470, 240, 26, IDC_CB_AUTOSAVE);

            settingsAdd(hwnd, L"STATIC", L"改动即时生效并自动保存", SS_LEFT, 18, 518, 240, 26, 0);
            settingsAdd(hwnd, L"BUTTON", L"关闭", BS_PUSHBUTTON, SET_W - 32 - 96, 514, 96, 32, IDC_BTN_CLOSE);
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            bool changed = true;
            switch (id) {
                case IDC_RB_SIZE3: case IDC_RB_SIZE4: case IDC_RB_SIZE5: {
                    app->selectedSize = 3 + (id - IDC_RB_SIZE3);
                    if (app->phase == PHASE_RUNNING) {
                        wchar_t b2[120];
                        std::swprintf(b2, 120, L"已改为 %d×%d，点「重新开始」按新大小开局",
                                      app->selectedSize, app->selectedSize);
                        setStatus(*app, b2);
                    } else {
                        wchar_t b2[120];
                        std::swprintf(b2, 120, L"已选择 %d×%d 棋盘，点「开始游戏」",
                                      app->selectedSize, app->selectedSize);
                        setStatus(*app, b2);
                    }
                    break;
                }
                case IDC_RB_L0: case IDC_RB_L1: case IDC_RB_L2:
                case IDC_RB_L3: case IDC_RB_L4: case IDC_RB_L5: {
                    cancelAiSearch(*app);
                    app->aiLevel = id - IDC_RB_L0;
                    const AiLevel& lv = AI_LEVELS[app->aiLevel];
                    wchar_t b2[240];
                    const int cores = effectiveThreads();
                    if (lv.depth == 0)
                        std::swprintf(b2, 240, L"AI 深度「%ls」：%d 线程并行 + 自适应加深，会把 %d 个核心吃满",
                                      lv.name, cores, cores);
                    else if (lv.depth >= 6)
                        std::swprintf(b2, 240, L"AI 深度「%ls」：%d 线程并行；开阔局面可能要算十几秒，残局才跑得满 %d 层",
                                      lv.name, cores, lv.depth);
                    else if (lv.depth >= 4)
                        std::swprintf(b2, 240, L"AI 深度「%ls」：固定 %d 层 + %d 线程并行",
                                      lv.name, lv.depth, cores);
                    else
                        std::swprintf(b2, 240, L"AI 深度「%ls」：固定 %d 层（单线程最快）",
                                      lv.name, lv.depth);
                    setStatus(*app, b2);
                    break;
                }
                case IDC_CB_ONLY2: {
                    const bool on = IsDlgButtonChecked(hwnd, IDC_CB_ONLY2) == BST_CHECKED;
                    if (on) CheckDlgButton(hwnd, IDC_CB_ONLY4, BST_UNCHECKED);
                    g_spawnMode = on ? SPAWN_ONLY2 : SPAWN_RANDOM;
                    break;
                }
                case IDC_CB_ONLY4: {
                    const bool on = IsDlgButtonChecked(hwnd, IDC_CB_ONLY4) == BST_CHECKED;
                    if (on) CheckDlgButton(hwnd, IDC_CB_ONLY2, BST_UNCHECKED);
                    g_spawnMode = on ? SPAWN_ONLY4 : SPAWN_RANDOM;
                    break;
                }
                case IDC_CB_AUTOSAVE:
                    app->autosave = (IsDlgButtonChecked(hwnd, IDC_CB_AUTOSAVE) == BST_CHECKED);
                    appSaveGame(*app, false);
                    break;
                case IDC_BTN_SAVE:
                    appSaveGame(*app, true);
                    settingsRefreshTexts(*app);
                    break;
                case IDC_BTN_LOAD:
                    appLoadGame(*app, true);
                    settingsSyncFromApp(*app);
                    break;
                case IDC_BTN_CLOSE:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    changed = false;
                    break;
            }
            if (changed) {
                refresh(*app);
                appSaveGame(*app, false);       // 配置改动立刻落盘
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, CLR_BG);
            SetTextColor(dc, CLR_DARKTXT);
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)g_setBg;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            app->settingsHwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 窗口过程 ----
static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        AppState* a = (AppState*)cs->lpCreateParams;
        a->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)a);
        return DefWindowProcW(hwnd, msg, wp, lp);   // 交给默认处理，窗口标题才会生效
    }

    AppState* app = (AppState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        // 支持 WM_PRINTCLIENT / PrintWindow 抓图（截图、录屏、远程查看都能拿到画面）
        case WM_PRINTCLIENT: {
            HDC dc = (HDC)wp;
            ensureMemDC(*app);
            paintAll(*app, app->memDC);
            blitTo(dc, *app);
            flushTexts(dc);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            ensureMemDC(*app);
            paintAll(*app, app->memDC);
            blitTo(dc, *app);
            flushTexts(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            onLButtonDown(*app, (short)LOWORD(lp), (short)HIWORD(lp));
            return 0;

        case WM_MOUSEMOVE:
            onMouseMove(*app, (short)LOWORD(lp), (short)HIWORD(lp));
            return 0;

        case WM_LBUTTONUP:
            onLButtonUp(*app);
            return 0;

        case WM_KEYDOWN:
            onKeyDown(*app, (int)wp);
            return 0;

        // AI 工作线程算完了
        case WM_APP_AI_DONE:
            finishAiSearch(*app);
            return 0;

        case WM_DESTROY:
            if (app->speedEdit) { DestroyWindow(app->speedEdit); app->speedEdit = nullptr; }
            appSaveGame(*app, false);              // 退出前存一次
            cancelAiSearch(*app);
            if (app->memDC) { DeleteDC(app->memDC); app->memDC = nullptr; }
            if (app->memBmp) { DeleteObject(app->memBmp); app->memBmp = nullptr; }
            destroyFonts(*app);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int runGui(HINSTANCE hInst, int nCmdShow) {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    HICON appIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));      // app.rc 里的图标
    wc.hIcon = appIcon ? appIcon : LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = nullptr;              // 全部自绘
    wc.lpszClassName = L"Game2048WindowClass";
    if (!RegisterClassExW(&wc)) return 1;

    WNDCLASSEXW wc2;
    ZeroMemory(&wc2, sizeof(wc2));
    wc2.cbSize = sizeof(wc2);
    wc2.style = CS_HREDRAW | CS_VREDRAW;
    wc2.lpfnWndProc = settingsProc;
    wc2.hInstance = hInst;
    wc2.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc2.hIcon = wc.hIcon;
    wc2.hIconSm = wc.hIconSm;
    wc2.hbrBackground = CreateSolidBrush(CLR_BG);
    wc2.lpszClassName = L"Game2048SettingsClass";
    RegisterClassExW(&wc2);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, CLIENT_W, CLIENT_H};
    AdjustWindowRect(&r, style, FALSE);

    AppState* app = new AppState();
    app->rng.seed((unsigned)(GetTickCount() ^ (unsigned)std::chrono::steady_clock::now()
                                                       .time_since_epoch()
                                                       .count()));
    app->lastMoveTick = std::chrono::steady_clock::now();
    app->lastSaveTick = std::chrono::steady_clock::now();
    warmUpWeights();                 // 多线程搜索前先算好权重表，避免数据竞争
    createFonts(*app);
    layout(*app);
    app->status = L"点「配置 / 设置」选棋盘大小、AI 深度，然后点「开始游戏」";

    HWND hwnd = CreateWindowExW(0, L"Game2048WindowClass", L"2048 · 蛇形算法 AI",
                                style, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, hInst, app);
    if (!hwnd) { delete app; return 1; }

    // 标题栏/任务栏图标（class 里设过一次，这里再确保大图标也生效）
    if (wc.hIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    }

    // 启动时自动读取存档（配置一定读，棋局看是否勾了「启动时自动读取存档」）
    if (app->autosave) {
        if (appLoadGame(*app, false)) {
            wchar_t buf[200];
            std::swprintf(buf, 200, L"已自动读取上次存档：%d×%d，%d 分 / %d 步",
                          app->game.n, app->game.n, app->game.score, app->game.moves);
            setStatus(*app, buf);
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    auto lastMove = std::chrono::steady_clock::now();
    bool quit = false;

    while (!quit) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else if (app->aiOn && app->phase == PHASE_RUNNING) {
            if (app->aiBusy.load()) {
                // 搜索在工作线程里跑（最高档要几秒），这里只等结果消息，界面照常响应
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
            } else {
                const double delay = (double)aiDelayMs(*app);
                const double elapsed = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - app->lastMoveTick)
                                           .count();
                if (elapsed >= delay) {
                    startAiSearch(*app);
                } else {
                    DWORD wait = (DWORD)std::max(1.0, delay - elapsed);
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT);
                }
            }
        } else {
            WaitMessage();
        }
    }

    cancelAiSearch(*app);        // 确保工作线程已结束，再释放状态
    g_pool.stopAll();            // 关闭搜索线程池
    delete app;
    return 0;
}

#endif  // !CONSOLE_BUILD

// ============================================================================
//  四、入口
// ============================================================================

struct CmdLine {
    bool bench = false;
    bool sweep = false;
    int size = 4;
    int games = 30;
};

// 手工切分命令行（避免依赖 CommandLineToArgvW / shell32）
static std::vector<std::wstring> splitArgs(const wchar_t* cmd) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool inQuote = false;
    for (const wchar_t* p = cmd; *p; ++p) {
        if (*p == L'"') {
            inQuote = !inQuote;
        } else if ((*p == L' ' || *p == L'\t') && !inQuote) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(*p);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static CmdLine parseCmdLine() {
    CmdLine c;
    std::vector<std::wstring> a = splitArgs(GetCommandLineW());
    for (size_t i = 1; i < a.size(); ++i) {
        if (a[i] == L"--bench" || a[i] == L"--sweep") {
            c.bench = true;
            c.sweep = (a[i] == L"--sweep");
            if (i + 1 < a.size() && a[i + 1][0] != L'-') c.size = _wtoi(a[++i].c_str());
            if (i + 1 < a.size() && a[i + 1][0] != L'-') c.games = _wtoi(a[++i].c_str());
        } else if (a[i] == L"--think" && i + 1 < a.size()) {
            g_thinkBudgetMs = _wtof(a[++i].c_str());
            if (g_thinkBudgetMs < 1.0) g_thinkBudgetMs = 1.0;
        } else if (a[i] == L"--empty" && i + 1 < a.size()) {
            g_emptyBonus = _wtof(a[++i].c_str());
        } else if (a[i] == L"--merge" && i + 1 < a.size()) {
            g_mergeBonus = _wtof(a[++i].c_str());
        } else if (a[i] == L"--base" && i + 1 < a.size()) {
            g_snakeBase = _wtof(a[++i].c_str());
            if (g_snakeBase < 1.5) g_snakeBase = 1.5;
        } else if (a[i] == L"--cutoff" && i + 1 < a.size()) {
            g_probCutoff = _wtof(a[++i].c_str());
        } else if (a[i] == L"--spawn4" && i + 1 < a.size()) {
            g_useSpawn4 = (_wtoi(a[++i].c_str()) != 0);
        } else if (a[i] == L"--threads" && i + 1 < a.size()) {
            g_threads = _wtoi(a[++i].c_str());
            if (g_threads < 0) g_threads = 0;      // 0 = 自动用满所有核心
        } else if (a[i] == L"--depth" && i + 1 < a.size()) {
            g_fixedDepth = _wtoi(a[++i].c_str());
        }
    }
    if (c.size < 3) c.size = 3;
    if (c.size > MAXN) c.size = MAXN;
    if (c.games < 1) c.games = 1;
    return c;
}

#ifdef CONSOLE_BUILD

int main() {
    CmdLine c = parseCmdLine();
    if (c.sweep) {
        runSweep(c.size, c.games);
        g_pool.stopAll();
        return 0;
    }
    if (c.bench) {
        runBench(c.size, c.games, L"bench_result.txt");
        g_pool.stopAll();
        return 0;
    }
    std::printf("这是无界面压测版；游戏请运行 game2048.exe\n");
    std::printf("用法： game2048_bench.exe --bench [边长 3/4/5] [局数] [--think 毫秒]\n");
    std::printf("       game2048_bench.exe --sweep [边长] [每组局数]\n");
    return 0;
}

#else

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    CmdLine c = parseCmdLine();
    if (c.bench) {
        // 无界面压测：结果同时写到 bench_result.txt（便于重定向/查看）
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
        }
        if (c.sweep) runSweep(c.size, c.games);
        else runBench(c.size, c.games, L"bench_result.txt");
        g_pool.stopAll();
        return 0;
    }
    return runGui(hInst, nCmdShow);
}

#endif



