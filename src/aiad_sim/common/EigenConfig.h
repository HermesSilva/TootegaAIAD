// The only way to include Eigen in Tootega AIAD.
//
// Project Chrono extends Eigen's classes through EIGEN_MATRIXBASE_PLUGIN and EIGEN_SPARSEMATRIX_PLUGIN
// (chrono/core/ChMatrix.h). Every translation unit linked into the same binary must see the same
// definitions, or the linker merges different versions of the Eigen templates (ODR) and the program
// crashes. Include Eigen through this header, never directly.
#pragma once

#include <chrono/core/ChMatrix.h>

#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
