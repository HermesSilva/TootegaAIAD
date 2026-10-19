// Spike S8 (milestone M1.1): Project Chrono 10 core, static, MSVC v145.
// Crank-slider of a single-cylinder engine driven at constant speed; the piston position must match
// the closed-form kinematics x(theta) = r cos(theta) + sqrt(l^2 - r^2 sin^2(theta)).
// Mechanism in the XY plane, rotation axes along Z, piston sliding along X:
//   crank-ground: rotation motor (revolute about Z, imposed speed)
//   crank-rod:    revolute
//   rod-piston:   point-on-line, line along Z (x, y coincide; avoids the planar redundancy)
//   piston-ground: prismatic along X
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChLinkLock.h>
#include <chrono/physics/ChLinkMotorRotationSpeed.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono/solver/ChDirectSolverLS.h>

#include <cmath>
#include <cstdio>

using namespace chrono;

int main()
{
    const double r = 0.030;               // crank radius [m] (60 mm stroke)
    const double l = 0.100;               // connecting rod length [m]
    const double omega = 2 * CH_PI * 50;  // 3000 rpm [rad/s]

    ChSystemNSC sys;
    sys.SetGravitationalAcceleration(ChVector3d(0, 0, 0));
    auto solver = chrono_types::make_shared<ChSolverSparseQR>();
    sys.SetSolver(solver);

    auto ground = chrono_types::make_shared<ChBody>();
    ground->SetFixed(true);
    sys.AddBody(ground);

    auto crank = chrono_types::make_shared<ChBody>();
    crank->SetPos(ChVector3d(r / 2, 0, 0));
    crank->SetMass(2.0);
    crank->SetInertiaXX(ChVector3d(1e-3, 1e-3, 2e-3));
    sys.AddBody(crank);

    auto rod = chrono_types::make_shared<ChBody>();
    rod->SetPos(ChVector3d(r + l / 2, 0, 0));
    rod->SetMass(0.4);
    rod->SetInertiaXX(ChVector3d(1e-5, 4e-4, 4e-4));
    sys.AddBody(rod);

    auto piston = chrono_types::make_shared<ChBody>();
    piston->SetPos(ChVector3d(r + l, 0, 0));
    piston->SetMass(0.3);
    piston->SetInertiaXX(ChVector3d(1e-4, 1e-4, 1e-4));
    sys.AddBody(piston);

    auto motor = chrono_types::make_shared<ChLinkMotorRotationSpeed>();
    motor->Initialize(crank, ground, ChFrame<>(ChVector3d(0, 0, 0)));
    motor->SetSpeedFunction(chrono_types::make_shared<ChFunctionConst>(omega));
    sys.AddLink(motor);

    auto crankPin = chrono_types::make_shared<ChLinkLockRevolute>();
    crankPin->Initialize(crank, rod, ChFrame<>(ChVector3d(r, 0, 0)));
    sys.AddLink(crankPin);

    // Point-line slides along the marker X axis: turn X onto Z.
    auto wristPin = chrono_types::make_shared<ChLinkLockPointLine>();
    wristPin->Initialize(rod, piston, ChFrame<>(ChVector3d(r + l, 0, 0), QuatFromAngleY(-CH_PI_2)));
    sys.AddLink(wristPin);

    // Prismatic slides along the marker Z axis: turn Z onto X.
    auto bore = chrono_types::make_shared<ChLinkLockPrismatic>();
    bore->Initialize(piston, ground, ChFrame<>(ChVector3d(r + l, 0, 0), QuatFromAngleY(CH_PI_2)));
    sys.AddLink(bore);

    const double dt = 1e-5, tEnd = 2 * (2 * CH_PI / omega);  // two revolutions
    double maxErr = 0.0;
    int steps = 0;
    while (sys.GetChTime() < tEnd - dt / 2)
    {
        sys.DoStepDynamics(dt);
        ++steps;
        const double theta = motor->GetMotorAngle();
        const double s = std::sin(theta);
        const double exact = r * std::cos(theta) + std::sqrt(l * l - r * r * s * s);
        maxErr = std::max(maxErr, std::abs(piston->GetPos().x() - exact));
    }
    const double thetaEnd = motor->GetMotorAngle();
    std::printf("steps %d, t = %.6f s, crank angle %.6f rad (expected %.6f)\n", steps, sys.GetChTime(), thetaEnd,
                omega * sys.GetChTime());
    std::printf("max |x_piston - x_exact| = %.3e m (stroke %.3f m)\n", maxErr, 2 * r);
    const bool ok = maxErr < 1e-6 && std::abs(thetaEnd - omega * sys.GetChTime()) < 1e-6;
    std::printf("%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
