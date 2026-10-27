// =============================================================================
// chrono_rt_bench — real-time cost vs. fidelity for off-road vehicles on SCM soil
//
// Runs one Chrono vehicle (HMMWV wheeled, or M113 tracked) over a flat SCM soil
// patch with a scripted, deterministic driver, as fast as it will go, and
// reports what a fixed-step real-time loop cares about: the distribution of
// wall time per step (worst case, not just mean) against the step size.
//
// Every knob is numerics or a backend choice (D35, D102), set on the command
// line: step size, soil grid spacing, SCM active domain, contact method,
// solver iterations, track-shoe type, tire type, thread count. Fidelity is
// measured on the motion-cue signal at the driver's seat (specific force,
// angular velocity, angular acceleration in the chassis frame), written per
// step to a CSV and compared against a reference run by tools/compare_cues.py.
//
// Run with --help for the options.
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "chrono/core/ChDataPath.h"
#include "chrono/physics/ChSystem.h"
#include "chrono/solver/ChIterativeSolver.h"

#include "chrono_vehicle/ChVehicleDataPath.h"
#include "chrono_vehicle/terrain/SCMTerrain.h"

#include "chrono_models/vehicle/hmmwv/HMMWV.h"
#include "chrono_models/vehicle/m113/M113.h"

using namespace chrono;
using namespace chrono::vehicle;

// -----------------------------------------------------------------------------
// Options
// -----------------------------------------------------------------------------

struct Options {
    std::string vehicle = "hmmwv";  // hmmwv | m113
    std::string tag;                // free label copied into the results row

    double step = -1;        // [s] integration step = the real-time tick; <0 selects a per-vehicle default
    double duration = 12.0;  // [s] simulated time, including settle
    double settle = 1.0;     // [s] inputs held at zero; excluded from step statistics

    double grid = 0.05;        // [m] SCM grid spacing
    double patch_len = 120.0;  // [m] SCM patch length (x)
    double patch_wid = 16.0;   // [m] SCM patch width (y)
    bool active_domain = true;
    bool bulldozing = false;

    std::string contact = "smc";  // nsc | smc
    int iterations = 150;  // iterative-solver cap (NSC)
    int threads = 1;

    std::string shoe = "single-pin";  // single-pin | double-pin | band-bushing | band-ancf
    std::string tire = "rigid";       // rigid | rigid-mesh

    double throttle = 0.5;  // steady throttle after the ramp
    double steer = 0.3;     // sine steering amplitude
    double z0 = -1.0;       // initial chassis height [m]; <0 selects a per-vehicle default

    std::string cues;     // per-step cue CSV (optional)
    std::string results;  // results CSV to append one row to (optional)
};

static void PrintUsage() {
    std::cout <<
        R"(chrono_rt_bench [options]

Vehicle and scenario
  --vehicle hmmwv|m113     vehicle (default hmmwv)
  --duration S             simulated seconds, including settle (default 12)
  --settle S               seconds with zero inputs, excluded from stats (default 1)
  --throttle X             steady throttle 0..1 (default 0.5)
  --steer X                sine steering amplitude 0..1 (default 0.3)
  --z0 M                   initial chassis height (default: per vehicle)

Knobs (numerics / backend)
  --step S                 step size = real-time tick (default 0.001 hmmwv, 0.0005 m113)
  --grid M                 SCM grid spacing (default 0.05)
  --patch-len M            SCM patch length (default 120)
  --patch-wid M            SCM patch width (default 16)
  --active-domain | --no-active-domain   SCM active domain around the chassis (default on)
  --bulldozing | --no-bulldozing         SCM bulldozing (default off)
  --contact nsc|smc        contact method (default smc)
  --iterations N           iterative-solver cap for NSC (default 150)
  --threads N              Chrono, collision and Eigen threads (default 1)
  --shoe single-pin|double-pin|band-bushing|band-ancf   M113 track shoe (default single-pin)
  --tire rigid|rigid-mesh  HMMWV tire (default rigid)

Output
  --cues FILE              write the per-step cue signal CSV
  --results FILE           append one summary row (header written if the file is new)
  --tag TEXT               label copied into the results row
)";
}

static bool ParseArgs(int argc, char* argv[], Options& o) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            PrintUsage();
            std::exit(0);
        } else if (a == "--vehicle") o.vehicle = next("--vehicle");
        else if (a == "--tag") o.tag = next("--tag");
        else if (a == "--step") o.step = std::stod(next("--step"));
        else if (a == "--duration") o.duration = std::stod(next("--duration"));
        else if (a == "--settle") o.settle = std::stod(next("--settle"));
        else if (a == "--grid") o.grid = std::stod(next("--grid"));
        else if (a == "--patch-len") o.patch_len = std::stod(next("--patch-len"));
        else if (a == "--patch-wid") o.patch_wid = std::stod(next("--patch-wid"));
        else if (a == "--active-domain") o.active_domain = true;
        else if (a == "--no-active-domain") o.active_domain = false;
        else if (a == "--bulldozing") o.bulldozing = true;
        else if (a == "--no-bulldozing") o.bulldozing = false;
        else if (a == "--contact") o.contact = next("--contact");
        else if (a == "--iterations") o.iterations = std::stoi(next("--iterations"));
        else if (a == "--threads") o.threads = std::stoi(next("--threads"));
        else if (a == "--shoe") o.shoe = next("--shoe");
        else if (a == "--tire") o.tire = next("--tire");
        else if (a == "--throttle") o.throttle = std::stod(next("--throttle"));
        else if (a == "--steer") o.steer = std::stod(next("--steer"));
        else if (a == "--z0") o.z0 = std::stod(next("--z0"));
        else if (a == "--cues") o.cues = next("--cues");
        else if (a == "--results") o.results = next("--results");
        else {
            std::cerr << "unknown option: " << a << "  (try --help)\n";
            return false;
        }
    }
    if (o.vehicle != "hmmwv" && o.vehicle != "m113") {
        std::cerr << "--vehicle must be hmmwv or m113\n";
        return false;
    }
    // M113 defaults: SMC diverges at 1 ms with individual track shoes; NSC needs a small step and
    // many iterations to keep the sprocket from slipping on the shoes, and is much slower.
    if (o.step < 0)
        o.step = (o.vehicle == "hmmwv") ? 1e-3 : 5e-4;
    if (o.contact != "nsc" && o.contact != "smc") {
        std::cerr << "--contact must be nsc or smc\n";
        return false;
    }
    if (o.z0 < 0)
        o.z0 = (o.vehicle == "hmmwv") ? 0.5 : 0.8;
    return true;
}

// -----------------------------------------------------------------------------
// Vehicle rigs: one interface over the wheeled and tracked model wrappers
// -----------------------------------------------------------------------------

class Rig {
  public:
    virtual ~Rig() = default;
    virtual ChSystem* System() const = 0;
    virtual ChVehicle& Vehicle() const = 0;
    virtual void Synchronize(double t, const DriverInputs& in, const ChTerrain& terrain) = 0;
    virtual void Advance(double step) = 0;
    ChChassis& Chassis() const { return *Vehicle().GetChassis(); }

    // One-line powertrain state, for sanity-checking that the vehicle is actually driven.
    virtual std::string Diagnostics() const {
        char buf[160];
        auto engine = Vehicle().GetEngine();
        auto trans = Vehicle().GetTransmission();
        std::snprintf(buf, sizeof(buf), "engine %.0f rpm, %.0f N-m   gear %d",
                      engine ? engine->GetMotorSpeed() * 30 / CH_PI : 0.0,
                      engine ? engine->GetOutputMotorshaftTorque() : 0.0, trans ? trans->GetCurrentGear() : 0);
        return buf;
    }
};

static ChContactMethod ContactMethod(const Options& o) {
    return o.contact == "nsc" ? ChContactMethod::NSC : ChContactMethod::SMC;
}

class HmmwvRig : public Rig {
  public:
    explicit HmmwvRig(const Options& o) {
        m_model.SetContactMethod(ContactMethod(o));
        m_model.SetCollisionSystemType(ChCollisionSystem::Type::BULLET);
        m_model.SetChassisFixed(false);
        m_model.SetChassisCollisionType(CollisionType::NONE);
        m_model.SetInitPosition(ChCoordsys<>(ChVector3d(-o.patch_len / 2 + 8, 0, o.z0), QUNIT));
        m_model.SetEngineType(EngineModelType::SHAFTS);
        m_model.SetTransmissionType(TransmissionModelType::AUTOMATIC_SHAFTS);
        m_model.SetDriveType(DrivelineTypeWV::AWD);
        m_model.SetTireType(o.tire == "rigid-mesh" ? TireModelType::RIGID_MESH : TireModelType::RIGID);
        m_model.SetTireStepSize(o.step);
        m_model.Initialize();

        m_model.SetChassisVisualizationType(VisualizationType::NONE);
        m_model.SetSuspensionVisualizationType(VisualizationType::NONE);
        m_model.SetSteeringVisualizationType(VisualizationType::NONE);
        m_model.SetWheelVisualizationType(VisualizationType::NONE);
        m_model.SetTireVisualizationType(VisualizationType::NONE);
    }
    ChSystem* System() const override { return m_model.GetSystem(); }
    ChVehicle& Vehicle() const override { return m_model.GetVehicle(); }
    void Synchronize(double t, const DriverInputs& in, const ChTerrain& terrain) override {
        m_model.Synchronize(t, in, terrain);
    }
    void Advance(double step) override { m_model.Advance(step); }

  private:
    hmmwv::HMMWV_Full m_model;
};

static TrackShoeType ShoeType(const std::string& s) {
    if (s == "double-pin") return TrackShoeType::DOUBLE_PIN;
    if (s == "band-bushing") return TrackShoeType::BAND_BUSHING;
    if (s == "band-ancf") return TrackShoeType::BAND_ANCF;
    return TrackShoeType::SINGLE_PIN;
}

class M113Rig : public Rig {
  public:
    explicit M113Rig(const Options& o) {
        m_model.SetContactMethod(ContactMethod(o));
        m_model.SetCollisionSystemType(ChCollisionSystem::Type::BULLET);
        m_model.SetChassisFixed(false);
        m_model.SetChassisCollisionType(CollisionType::NONE);
        m_model.SetTrackShoeType(ShoeType(o.shoe));
        m_model.SetDoublePinTrackShoeType(DoublePinTrackShoeType::ONE_CONNECTOR);
        m_model.SetDrivelineType(DrivelineTypeTV::BDS);
        m_model.SetBrakeType(BrakeType::SIMPLE);
        // Shaft-based powertrain with a torque converter. The simple-map variant ties engine speed
        // rigidly to the sprockets and stalls a vehicle starting from rest in soft soil.
        m_model.SetEngineType(EngineModelType::SHAFTS);
        m_model.SetTransmissionType(TransmissionModelType::AUTOMATIC_SHAFTS);
        m_model.SetInitPosition(ChCoordsys<>(ChVector3d(-o.patch_len / 2 + 8, 0, o.z0), QUNIT));
        m_model.Initialize();

        m_model.SetChassisVisualizationType(VisualizationType::NONE);
        m_model.SetSprocketVisualizationType(VisualizationType::NONE);
        m_model.SetIdlerVisualizationType(VisualizationType::NONE);
        m_model.SetSuspensionVisualizationType(VisualizationType::NONE);
        m_model.SetIdlerWheelVisualizationType(VisualizationType::NONE);
        m_model.SetRoadWheelVisualizationType(VisualizationType::NONE);
        m_model.SetTrackShoeVisualizationType(VisualizationType::NONE);
    }
    ChSystem* System() const override { return m_model.GetSystem(); }
    ChVehicle& Vehicle() const override { return m_model.GetVehicle(); }
    void Synchronize(double t, const DriverInputs& in, const ChTerrain&) override { m_model.Synchronize(t, in); }
    void Advance(double step) override { m_model.Advance(step); }
    std::string Diagnostics() const override {
        char buf[96];
        auto& v = m_model.GetVehicle();
        std::snprintf(buf, sizeof(buf), "   sprocket L %.2f R %.2f rad/s",
                      v.GetTrackAssembly(LEFT)->GetSprocket()->GetAxleSpeed(),
                      v.GetTrackAssembly(RIGHT)->GetSprocket()->GetAxleSpeed());
        return Rig::Diagnostics() + buf;
    }

  private:
    m113::M113 m_model;
};

// -----------------------------------------------------------------------------
// Scripted driver: a pure function of time, so runs are repeatable
// -----------------------------------------------------------------------------

static DriverInputs Script(double t, const Options& o) {
    DriverInputs in{0, 0, 0, 0};
    double tau = t - o.settle;
    if (tau <= 0)
        return in;
    in.m_throttle = o.throttle * std::min(1.0, tau / 1.0);  // 1 s ramp
    double ts = tau - 3.0;                                    // steer after 3 s of driving
    if (ts > 0)
        in.m_steering = o.steer * std::sin(2 * CH_PI * ts / 4.0);  // 4 s period
    return in;
}

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------

static double Percentile(std::vector<double> v, double p) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    size_t k = (size_t)std::ceil(p * (double)v.size());
    k = std::min(std::max<size_t>(k, 1), v.size());
    return v[k - 1];
}

// -----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    Options o;
    if (!ParseArgs(argc, argv, o))
        return 2;

    SetChronoDataPath(CHRONO_DATA_DIR);
    SetVehicleDataPath(CHRONO_VEHICLE_DATA_DIR);

    std::unique_ptr<Rig> rig;
    if (o.vehicle == "hmmwv")
        rig = std::make_unique<HmmwvRig>(o);
    else
        rig = std::make_unique<M113Rig>(o);

    ChSystem* sys = rig->System();
    sys->SetNumThreads(o.threads, o.threads, o.threads);
    if (o.contact == "nsc") {
        sys->SetSolverType(ChSolver::Type::BARZILAIBORWEIN);
        if (auto* it = sys->GetSolver()->AsIterative())
            it->SetMaxIterations(o.iterations);
    }

    // SCM soil patch. Parameters follow Chrono's SCM vehicle demos (a soft, frictional soil).
    SCMTerrain terrain(sys, false);
    terrain.SetSoilParameters(2e6,   // Bekker Kphi
                              0,     // Bekker Kc
                              1.1,   // Bekker n
                              0,     // Mohr cohesion [Pa]
                              30,    // Mohr friction angle [deg]
                              0.01,  // Janosi shear [m]
                              4e7,   // elastic stiffness [Pa/m]
                              3e4);  // damping [Pa s/m]
    terrain.EnableBulldozing(o.bulldozing);
    terrain.Initialize(o.patch_len, o.patch_wid, o.grid);

    auto chassis_body = rig->Chassis().GetBody();
    if (o.active_domain) {
        ChVector3d com = chassis_body->GetFrameCOMToRef().GetPos();
        ChVector3d dims = (o.vehicle == "hmmwv") ? ChVector3d(6.0, 3.0, 3.0) : ChVector3d(6.5, 3.5, 3.0);
        terrain.AddActiveDomain(chassis_body, ChVector3d(com.x(), com.y(), 0), dims);
    }

    // Cue reference point: the chassis's declared driver position.
    const ChVector3d driver_pos = rig->Chassis().GetLocalDriverCoordsys().pos;

    std::ofstream cue;
    if (!o.cues.empty()) {
        cue.open(o.cues);
        if (!cue) {
            std::cerr << "cannot open " << o.cues << "\n";
            return 1;
        }
        cue << "t,x,y,z,speed,fx,fy,fz,wx,wy,wz,ax,ay,az,throttle,steering,step_ms\n";
        cue.precision(9);
    }

    const long nsteps = (long)std::llround(o.duration / o.step);
    std::vector<double> step_ms;  // after settle
    step_ms.reserve(nsteps);
    double scm_ms_sum = 0;
    double wall_total_s = 0;
    long overruns = 0;

    std::cout << "chrono_rt_bench: " << o.vehicle << "  step=" << o.step << "  grid=" << o.grid
              << "  contact=" << o.contact << "  threads=" << o.threads
              << (o.vehicle == "m113" ? "  shoe=" + o.shoe : "  tire=" + o.tire)
              << "  active_domain=" << (o.active_domain ? "on" : "off") << std::endl;

    using clock = std::chrono::steady_clock;
    for (long i = 0; i < nsteps; i++) {
        double t = sys->GetChTime();
        DriverInputs in = Script(t, o);

        auto t0 = clock::now();
        terrain.Synchronize(t);
        rig->Synchronize(t, in, terrain);
        terrain.Advance(o.step);
        rig->Advance(o.step);
        auto t1 = clock::now();

        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        wall_total_s += ms * 1e-3;
        if (t >= o.settle) {
            step_ms.push_back(ms);
            if (ms > o.step * 1e3)
                overruns++;
            // Ray casting already includes the ray intersection tests.
            scm_ms_sum += terrain.GetTimerActiveDomains() + terrain.GetTimerRayCasting() +
                          terrain.GetTimerContactPatches() +
                          terrain.GetTimerContactForces() + terrain.GetTimerBulldozing();
        }

        const ChVector3d pos = rig->Vehicle().GetPos();
        if (!std::isfinite(pos.x()) || !std::isfinite(pos.z())) {
            std::cerr << "simulation diverged at t=" << t << "\n";
            return 1;
        }

        if (cue) {
            const auto& F = chassis_body->GetFrameRefToAbs();
            ChVector3d acc = rig->Chassis().GetPointAcceleration(driver_pos);
            ChVector3d f = F.TransformDirectionParentToLocal(acc - sys->GetGravitationalAcceleration());
            ChVector3d w = F.GetAngVelLocal();
            ChVector3d wd = F.GetAngAccLocal();
            cue << sys->GetChTime() << ',' << pos.x() << ',' << pos.y() << ',' << pos.z() << ','
                << rig->Vehicle().GetSpeed() << ',' << f.x() << ',' << f.y() << ',' << f.z() << ',' << w.x() << ','
                << w.y() << ',' << w.z() << ',' << wd.x() << ',' << wd.y() << ',' << wd.z() << ','
                << in.m_throttle << ',' << in.m_steering << ',' << ms << '\n';
        }
    }

    // ---- summary ----
    const double sim_s = sys->GetChTime();
    const double mean = step_ms.empty() ? 0 : std::accumulate(step_ms.begin(), step_ms.end(), 0.0) / step_ms.size();
    const double p50 = Percentile(step_ms, 0.50);
    const double p99 = Percentile(step_ms, 0.99);
    const double p999 = Percentile(step_ms, 0.999);
    const double mx = step_ms.empty() ? 0 : *std::max_element(step_ms.begin(), step_ms.end());
    const double scm_mean = step_ms.empty() ? 0 : scm_ms_sum / step_ms.size();
    const double budget = o.step * 1e3;
    const ChVector3d pos = rig->Vehicle().GetPos();

    std::printf("  sim %.3f s in %.3f s wall   RTF (wall/sim) %.3f\n", sim_s, wall_total_s, wall_total_s / sim_s);
    std::printf("  step ms (after settle, budget %.3f):  mean %.3f  p50 %.3f  p99 %.3f  p99.9 %.3f  max %.3f\n",
                budget, mean, p50, p99, p999, mx);
    std::printf("  overruns %ld / %zu   SCM share %.0f%%   real-time at p99.9: %s\n", overruns, step_ms.size(),
                mean > 0 ? 100 * scm_mean / mean : 0.0, p999 <= budget ? "yes" : "NO");
    std::printf("  final pos (%.17g, %.17g, %.17g)  speed %.3f m/s\n", pos.x(), pos.y(), pos.z(),
                rig->Vehicle().GetSpeed());
    std::printf("  %s\n", rig->Diagnostics().c_str());

    if (!o.results.empty()) {
        bool fresh = !std::ifstream(o.results).good();
        std::ofstream res(o.results, std::ios::app);
        if (!res) {
            std::cerr << "cannot open " << o.results << "\n";
            return 1;
        }
        if (fresh)
            res << "tag,vehicle,contact,step,grid,active_domain,bulldozing,shoe,tire,threads,iterations,"
                   "sim_s,wall_s,rtf,mean_ms,p50_ms,p99_ms,p999_ms,max_ms,overruns,steps,scm_mean_ms,"
                   "final_x,final_y,final_z,final_speed\n";
        res.precision(10);
        res << o.tag << ',' << o.vehicle << ',' << o.contact << ',' << o.step << ',' << o.grid << ','
            << o.active_domain << ',' << o.bulldozing << ',' << o.shoe << ',' << o.tire << ',' << o.threads << ','
            << o.iterations << ',' << sim_s << ',' << wall_total_s << ',' << wall_total_s / sim_s << ',' << mean << ','
            << p50 << ',' << p99 << ',' << p999 << ',' << mx << ',' << overruns << ',' << step_ms.size() << ','
            << scm_mean << ',' << pos.x() << ',' << pos.y() << ',' << pos.z() << ',' << rig->Vehicle().GetSpeed()
            << '\n';
    }
    return 0;
}
