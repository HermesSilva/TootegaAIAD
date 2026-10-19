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
