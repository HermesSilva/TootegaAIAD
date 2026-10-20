// The generic multibody solver against closed-form mechanics, described only as bodies, joints, motors and
// force elements: a physical pendulum (period), a motor-driven slider-crank (piston position), a gear train,
// a rack, a spring-mass (period), a cam with a flat follower (lift) and an end stop.
#include "mechanism/Multibody.h"

#include <cmath>
#include <cstdio>
#include <numbers>

namespace
{

int g_failures = 0;
#define CHECK(c)                                                                \
    do                                                                          \
    {                                                                           \
        if (!(c))                                                               \
        {                                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

constexpr double kPi = std::numbers::pi;

}  // namespace

int main()
{
    using namespace aiad::sim::mb;

    // Physical pendulum: uniform rod 1 m, 1 kg, hinged at one end about Z, released 5 deg off the
    // vertical under gravity -Y. T = 2 pi sqrt(2L / 3g) (small angle; 5 deg adds 0.05 %).
    {
        const double L = 1.0, m = 1.0, g = 9.81, a0 = 5.0 * kPi / 180.0;
        Model model;
        model.gravity = {0.0, -g, 0.0};
        Body rod;
        rod.name = "Rod";
        rod.mass = m;
        rod.centroid = {0.5 * L * std::sin(a0), -0.5 * L * std::cos(a0), 0.0};
        rod.inertia = {m * L * L / 12.0, 1e-6, m * L * L / 12.0, 0.0, 0.0, 0.0};
        model.bodies.push_back(rod);
        model.joints.push_back({"Hinge", JointType::Revolute, 0, -1, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {}});
        CHECK(Mobility(model) == 1);
        Multibody mb(model);
        const double dt = 1e-4;
        double previousW = 0.0, firstTurn = -1.0, period = -1.0;
        for (int i = 0; i < 40000 && period < 0.0; ++i)
        {
            mb.Step(dt);
            const double w = mb.State(0).angularVelocity[2];
            if (i > 10 && previousW > 0.0 && w <= 0.0)   // back at the release side: one period per turning point
            {
                if (firstTurn < 0.0)
                    firstTurn = mb.Time();
                else
                    period = mb.Time() - firstTurn;
            }
            previousW = w;
        }
        const double expected = 2.0 * kPi * std::sqrt(2.0 * L / (3.0 * g));
        std::printf("pendulum period %.4f s, closed form %.4f s\n", period, expected);
        CHECK(std::abs(period - expected) / expected < 0.005);
    }

    // Slider-crank: crank r = 0.03 m turned by a speed motor at 100 rad/s about Z, rod l = 0.1 m,
    // piston sliding along X. Piston x = r cos t + sqrt(l^2 - r^2 sin^2 t). Revolute joints everywhere
    // (planar, redundant in 3D: the solver tolerates it).
    {
        const double r = 0.03, l = 0.1, w = 100.0;
        Model model;
        auto body = [](const char* name, double mass, std::array<double, 3> c) {
            Body b;
            b.name = name;
            b.mass = mass;
            b.centroid = c;
            b.inertia = {mass * 1e-4, mass * 1e-4, mass * 1e-4, 0.0, 0.0, 0.0};
            return b;
        };
        model.bodies = {body("Crank", 2.0, {0.0, 0.0, 0.0}), body("Rod", 0.3, {r + l / 2, 0.0, 0.0}), body("Piston", 0.4, {r + l, 0.0, 0.0})};
        Motor drive;
        drive.mode = Motor::Mode::Speed;
        drive.value = w;
        model.joints = {{"Main", JointType::Revolute, 0, -1, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, drive},
                        {"CrankPin", JointType::Revolute, 1, 0, {r, 0.0, 0.0}, {0.0, 0.0, 1.0}, {}},
                        {"WristPin", JointType::Revolute, 2, 1, {r + l, 0.0, 0.0}, {0.0, 0.0, 1.0}, {}},
                        {"Bore", JointType::Prismatic, 2, -1, {r + l, 0.0, 0.0}, {1.0, 0.0, 0.0}, {}}};
        CHECK(Mobility(model) == 0);   // driven: the motor takes the one degree of freedom
        Multibody mb(model);
        double worst = 0.0;
        for (int i = 0; i < 2000; ++i)
        {
            mb.Step(1e-4);
            const double t = mb.Coordinate(0);
            const double expected = r * std::cos(t) + std::sqrt(l * l - r * r * std::sin(t) * std::sin(t));
            worst = std::max(worst, std::abs(mb.State(2).centroid[0] - expected));
        }
        std::printf("slider-crank: crank %.3f rad (motor %.3f), worst piston error %.2e m\n", mb.Coordinate(0), w * mb.Time(), worst);
        CHECK(worst < 1e-5);
        CHECK(std::abs(mb.Coordinate(0) - w * mb.Time()) < 1e-3);
    }

    auto wheel = [](const char* name, std::array<double, 3> c) {
        Body b;
        b.name = name;
        b.mass = 1.0;
        b.centroid = c;
        b.inertia = {1e-3, 1e-3, 1e-3, 0.0, 0.0, 0.0};
        return b;
    };
    Motor drive;
    drive.mode = Motor::Mode::Speed;

    // Gear train: driver (18 teeth) at 10 rad/s -> idler (16) -> output (36). Each mesh reverses the
    // direction: the output turns with the driver at 18/36 of its speed.
    {
        Model model;
        model.bodies = {wheel("Driver", {0, 0, 0}), wheel("Idler", {0.04, 0, 0}), wheel("Output", {0.04, 0.06, 0})};
        drive.value = 10.0;
        model.joints = {{"A", JointType::Revolute, 0, -1, {0, 0, 0}, {0, 0, 1}, drive},
                        {"I", JointType::Revolute, 1, -1, {0.04, 0, 0}, {0, 0, 1}, {}},
                        {"C", JointType::Revolute, 2, -1, {0.04, 0.06, 0}, {0, 0, 1}, {}}};
        model.couplings = {{"A-I", 0, 1, -18.0 / 16.0}, {"I-C", 1, 2, -16.0 / 36.0}};
        CHECK(Mobility(model) == 0);
        Multibody mb(model);
        for (int i = 0; i < 1000; ++i)
            mb.Step(1e-3);
        std::printf("gear train: driver %.4f rad, idler %.4f rad, output %.4f rad (expected 10, -11.25, 5)\n", mb.Coordinate(0), mb.Coordinate(1),
                    mb.Coordinate(2));
        CHECK(std::abs(mb.Coordinate(1) + 11.25) < 1e-3 && std::abs(mb.Coordinate(2) - 5.0) < 1e-3);
    }

    // The same train started away from the drawing: the idler at 0.5 rad, the output from the local time as a minute hand
    // (period 3600 s). They start there, the meshes do not drag them back, and from there they follow the driver.
    {
        Model model;
        model.bodies = {wheel("Driver", {0, 0, 0}), wheel("Idler", {0.04, 0, 0}), wheel("Output", {0.04, 0.06, 0})};
        drive.value = 10.0;
        model.joints = {{"A", JointType::Revolute, 0, -1, {0, 0, 0}, {0, 0, 1}, drive},
                        {"I", JointType::Revolute, 1, -1, {0.04, 0, 0}, {0, 0, 1}, {}},
                        {"C", JointType::Revolute, 2, -1, {0.04, 0.06, 0}, {0, 0, 1}, {}}};
        model.joints[1].start = 0.5;
        model.joints[2].clockPeriod = 3600.0;
        model.couplings = {{"A-I", 0, 1, -18.0 / 16.0}, {"I-C", 1, 2, -16.0 / 36.0}};
        const double minute = 2.0 * std::numbers::pi * std::fmod(LocalClockSeconds(), 3600.0) / 3600.0;
        Multibody mb(model);
        const double idler0 = mb.Coordinate(1), output0 = mb.Coordinate(2);
        for (int i = 0; i < 1000; ++i)
            mb.Step(1e-3);
        std::printf("started: idler %.4f -> %.4f rad, output %.4f (local time %.4f) -> %.4f rad\n", idler0, mb.Coordinate(1), output0, minute,
                    mb.Coordinate(2));
        CHECK(std::abs(idler0 - 0.5) < 1e-9 && std::abs(output0 - minute) < 1e-3);
        CHECK(std::abs(mb.Coordinate(1) - (0.5 - 11.25)) < 1e-3 && std::abs(mb.Coordinate(2) - (output0 + 5.0)) < 1e-3);
    }

    // Rack and pinion: pinion (pitch radius 25 mm) at 2 rad/s, the rack slides 25 mm per radian.
    {
        Model model;
        model.bodies = {wheel("Pinion", {0, 0, 0}), wheel("Rack", {0, -0.025, 0})};
        drive.value = 2.0;
        model.joints = {{"Pinion", JointType::Revolute, 0, -1, {0, 0, 0}, {0, 0, 1}, drive},
                        {"Rack", JointType::Prismatic, 1, -1, {0, -0.025, 0}, {1, 0, 0}, {}}};
        model.couplings = {{"Mesh", 0, 1, 0.025}};
        Multibody mb(model);
        for (int i = 0; i < 500; ++i)
            mb.Step(1e-3);
        std::printf("rack: %.5f m after %.3f rad (expected %.5f)\n", mb.Coordinate(1), mb.Coordinate(0), 0.025 * mb.Coordinate(0));
        CHECK(std::abs(mb.Coordinate(1) - 0.025) < 1e-5);
    }

    // Spring-mass: 1 kg on a slide, k = 100 N/m, released 10 mm from its free length: T = 2 pi sqrt(m / k).
    {
        Model model;
        model.bodies = {wheel("Mass", {0.1, 0, 0})};
        model.joints = {{"Slide", JointType::Prismatic, 0, -1, {0.1, 0, 0}, {1, 0, 0}, {}}};
        model.springs = {{"Spring", -1, 0, {0, 0, 0}, {0.1, 0, 0}, 100.0, 0.11, 0.0}};
        Multibody mb(model);
        double previous = 0.0, first = -1.0, period = -1.0;
        for (int i = 0; i < 20000 && period < 0.0; ++i)
        {
            mb.Step(1e-4);
            const double v = mb.Rate(0);
            if (i > 10 && previous > 0.0 && v <= 0.0)
            {
                if (first < 0.0)
                    first = mb.Time();
                else
                    period = mb.Time() - first;
            }
            previous = v;
        }
        std::printf("spring-mass period %.4f s, closed form %.4f s\n", period, 2.0 * kPi * std::sqrt(1.0 / 100.0));
        CHECK(std::abs(period - 2.0 * kPi * 0.1) < 2e-3);
    }

    // Cam and flat follower: an eccentric disc (radius 20 mm, 5 mm off its axis) turning at 10 rad/s under
    // a follower held on it by a preloaded spring: follower = 5 (cos t - 1) mm (the lift of the profile).
    {
        const double R = 0.02, e = 0.005;
        Model model;
        model.bodies = {wheel("Cam", {0, 0, 0}), wheel("Follower", {R + e + 0.01, 0, 0})};
        model.bodies[1].mass = 0.1;
        drive.value = 10.0;
        model.joints = {{"Cam", JointType::Revolute, 0, -1, {0, 0, 0}, {0, 0, 1}, drive},
                        {"Guide", JointType::Prismatic, 1, -1, {R + e, 0, 0}, {1, 0, 0}, {}}};
        Contact contact;
        contact.name = "Lobe";
        contact.body = 0;
        contact.follower = 1;
        for (int i = 0; i < 720; ++i)
            contact.profile.push_back({e + R * std::cos(i * kPi / 360.0), R * std::sin(i * kPi / 360.0), 0.0});
        contact.point = {R + e, 0, 0};
        contact.normal = {-1, 0, 0};
        contact.radius = 0.015;
        model.contacts = {contact};
        model.springs = {{"Return", 1, -1, {R + e, 0, 0}, {R + e + 0.05, 0, 0}, 2000.0, 0.075, 0.0}};   // 50 N preload
        Multibody mb(model);
        const double dt = std::min(1e-4, mb.StableStep());
        double worst = 0.0;
        for (int i = 0; mb.Time() < 1.0; ++i)
        {
            mb.Step(dt);
            if (mb.Time() > 0.05)
                worst = std::max(worst, std::abs(mb.Coordinate(1) - e * (std::cos(mb.Coordinate(0)) - 1.0)));
        }
        std::printf("cam follower: worst lift error %.1f um (step %.1e s), contact force %.1f N\n", worst * 1e6, dt, mb.ContactForce(0));
        CHECK(worst < 2e-5);
        CHECK(mb.ContactForce(0) > 29.0);   // the spring is at least 15 mm short of its free length
    }

    // A stop: a 10 N push against an end stop at 10 mm rests on it.
    {
        Model model;
        model.bodies = {wheel("Slider", {0, 0, 0})};
        Motor push;
        push.mode = Motor::Mode::Torque;
        push.value = 10.0;
        model.joints = {{"Slide", JointType::Prismatic, 0, -1, {0, 0, 0}, {1, 0, 0}, push, std::nullopt, 0.01}};
        Multibody mb(model);
        const double dt = std::min(1e-4, mb.StableStep());
        while (mb.Time() < 0.5)
            mb.Step(dt);
        std::printf("stop: slider at %.4f mm (stop at 10 mm), speed %.2e m/s\n", mb.Coordinate(0) * 1e3, mb.Rate(0));
        CHECK(std::abs(mb.Coordinate(0) - 0.01) < 1e-5 && std::abs(mb.Rate(0)) < 1e-3);
    }

    // A watch balance: 3.4e-9 kg m^2 on its staff, a hairspring tuned to 2.5 Hz (18000 beats per hour), relaxed
    // at 0.5 rad: it swings between 0 and 1 rad with T = 2 pi sqrt(I / k).
    {
        const double I = 3.4e-9, f = 2.5, k = I * std::pow(2.0 * kPi * f, 2);
        Model model;
        Body balance;
        balance.name = "Balance";
        balance.mass = 1.1e-4;
        balance.inertia = {I / 2.0, I / 2.0, I, 0.0, 0.0, 0.0};
        model.bodies = {balance};
        model.joints = {{"Staff", JointType::Revolute, 0, -1, {0, 0, 0}, {0, 0, 1}, {}}};
        model.torsions = {{"Hairspring", 0, k, 0.5, 0.0}};
        Multibody mb(model);
        const double dt = std::min(1e-4, mb.StableStep());
        double previous = 0.0, firstTurn = -1.0, period = -1.0, widest = 0.0;
        while (mb.Time() < 2.0 && period < 0.0)
        {
            mb.Step(dt);
            const double w = mb.Rate(0);
            widest = std::max(widest, mb.Coordinate(0));
            if (mb.Time() > 2.0 * dt && previous > 0.0 && w <= 0.0)
            {
                if (firstTurn < 0.0)
                    firstTurn = mb.Time();
                else
                    period = mb.Time() - firstTurn;
            }
            previous = w;
        }
        std::printf("balance: period %.4f s (closed form %.4f s), swings to %.3f rad\n", period, 1.0 / f, widest);
        CHECK(std::abs(period - 1.0 / f) < 0.002);
        CHECK(std::abs(widest - 1.0) < 0.01);
    }

    // Outlines that touch (watch scale): an arm 1..3 mm from its staff, 0.5 mm wide, turned by a torsion spring
    // (1e-6 N m at rest, relaxed at 1 rad) against a fixed block over it, 2..2.5 mm out and 0.5 mm up: it stops
    // where its upper edge reaches the block's outer corner, 0.5 cos t - 2.5 sin t = 0.25 (mm), and rests there:
    // the corner's force times its arm (2.5 cos t + 0.5 sin t mm) holds the spring's torque.
    {
        const double mm = 1e-3, k = 1e-6, free = 1.0;
        Model model;
        Body arm;
        arm.name = "Arm";
        arm.mass = 0.2 * 7850 * 1e-9;   // 2 x 0.5 x 0.2 mm of steel
        arm.centroid = {2 * mm, 0, 0};
        arm.inertia = {arm.mass * 0.25 * mm * mm / 12.0, arm.mass * 4 * mm * mm / 12.0, arm.mass * 4.25 * mm * mm / 12.0, 0, 0, 0};
        model.bodies = {arm};
        model.joints = {{"Staff", JointType::Revolute, 0, -1, {0, 0, 0}, {0, 0, 1}, {}}};
        model.torsions = {{"Spring", 0, k, free, 0.0}};
        Touch touch;
        touch.name = "Corner";
        touch.body0 = 0;
        touch.body1 = -1;
        touch.outline0 = {{{1 * mm, -0.25 * mm, 0}, {3 * mm, -0.25 * mm, 0}, {3 * mm, 0.25 * mm, 0}, {1 * mm, 0.25 * mm, 0}}};
        touch.outline1 = {{{2 * mm, 0.5 * mm, 0}, {2.5 * mm, 0.5 * mm, 0}, {2.5 * mm, 1 * mm, 0}, {2 * mm, 1 * mm, 0}}};
        model.touches = {touch};
        Multibody mb(model);
        const double dt = mb.StableStep();
        while (mb.Time() < 0.05)
            mb.Step(dt);
        double stop = 0.0;   // 0.5 cos t - 2.5 sin t = 0.25
        for (int i = 0; i < 60; ++i)
            stop -= (0.5 * std::cos(stop) - 2.5 * std::sin(stop) - 0.25) / (-0.5 * std::sin(stop) - 2.5 * std::cos(stop));
        const double force = k * (free - stop) / ((2.5 * std::cos(stop) + 0.5 * std::sin(stop)) * mm);
        std::printf("touch: arm stops at %.5f rad (corner at %.5f rad), force %.3e N (expected %.3e N), step %.1e s\n", mb.Coordinate(0), stop,
                    mb.TouchForce(0), force, dt);
        CHECK(std::abs(mb.Coordinate(0) - stop) < 2e-3);
        CHECK(std::abs(mb.TouchForce(0) - force) < 0.03 * force);
        CHECK(std::abs(mb.Rate(0)) < 0.5);
    }

    // A watch's fork (7.3e-11 kg m^2) thrown by a torsion spring (1e-6 N m, relaxed at 1 rad) at its banking, a stop
    // at 3.3 deg: it must not pass the banking by more than a fraction of a degree, nor bounce back with more
    // energy than it brought.
    {
        Model model;
        Body fork;
        fork.name = "Fork";
        fork.mass = 1.3e-5;
        fork.centroid = {0.002, 0, 0};
        fork.inertia = {1e-12, 2e-11, 7.3e-11 - 1.3e-5 * 4e-6, 0, 0, 0};
        model.bodies = {fork};
        model.joints = {{"Staff", JointType::Revolute, 0, -1, {0, 0, 0}, {0, 0, 1}, {}, std::nullopt, 0.0576}};
        model.torsions = {{"Push", 0, 1e-6, 1.0, 0.0}};
        Multibody mb(model);
        const double dt = std::min(1e-4, mb.StableStep());
        double furthest = 0.0, back = 1e9;
        while (mb.Time() < 0.05)
        {
            mb.Step(dt);
            furthest = std::max(furthest, mb.Coordinate(0));
            if (mb.Time() > 0.01)
                back = std::min(back, mb.Coordinate(0));
        }
        std::printf("banking: fork reaches %.3f deg (banking 3.3), afterwards no lower than %.3f deg, step %.1e s\n", furthest * 57.29578, back * 57.29578, dt);
        CHECK(furthest < 0.0576 + 0.01 && back > 0.0);
    }

    // A body with no joint is free: 6 degrees of freedom.
    {
        Model model;
        Body free;
        free.name = "Free";
        free.mass = 1.0;
        model.bodies.push_back(free);
        CHECK(Mobility(model) == 6);
    }

    std::printf(g_failures ? "multibody_test: %d failure(s)\n" : "multibody_test: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
