//==============================================================
// Copyright © 2019 Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

// ISO2DFD: Intel® oneAPI DPC++ Language Basics Using 2D-Finite-Difference-Wave
// Propagation
//
// ISO2DFD is a finite difference stencil kernel for solving the 2D acoustic
// isotropic wave equation. Kernels in this sample are implemented as 2nd order
// in space, 2nd order in time scheme without boundary conditions. Using Data
// Parallel C++, the sample will explicitly run on the GPU as well as CPU to
// calculate a result.  If successful, the output will include GPU device name.
//
// A complete online tutorial for this code sample can be found at :
// https://software.intel.com/en-us/articles/code-sample-two-dimensional-finite-difference-wave-propagation-in-isotropic-media-iso2dfd
//
// For comprehensive instructions regarding DPC++ Programming, go to
// https://software.intel.com/en-us/oneapi-programming-guide 
// and search based on relevant terms noted in the comments.
//
// DPC++ material used in this code sample:
//
// Basic structures of DPC++:
//   DPC++ Queues (including device selectors and exception handlers)
//   DPC++ Buffers and accessors (communicate data between the host and the device)
//   DPC++ Kernels (including parallel_for function and range<2> objects)
//

#include <fstream>
#include <iostream>
#include <CL/sycl.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <stdio.h>

using namespace cl::sycl;

/*
 * Parameters to define coefficients
 * HALF_LENGTH: Radius of the stencil
 * Sample source code is tested for HALF_LENGTH=1 resulting in
 * 2nd order Stencil finite difference kernel
 */

constexpr float DT = 0.002f;
constexpr float DXY = 20.0f;
constexpr unsigned int HALF_LENGTH = 1;

/*
 * Host-Code
 * Utility function to display input arguments
 */
void usage(std::string programName) {
  std::cout << " Incorrect parameters " << "\n";
  std::cout << " Usage: ";
  std::cout << programName << " n1 n2 Iterations " << "\n"
            << "\n";
  std::cout << " n1 n2      : Grid sizes for the stencil " << "\n";
  std::cout << " Iterations : No. of timesteps. " << "\n";
}

/*
 * Host-Code
 * Function used for initialization
 */
void initialize(float* ptr_prev, float* ptr_next, float* ptr_vel, size_t nRows,
                size_t nCols) {
  std::cout << "Initializing ... " << "\n";

  // Define source wavelet
  float wavelet[12] = {0.016387336, -0.041464937, -0.067372555, 0.386110067,
                       0.812723635, 0.416998396,  0.076488599,  -0.059434419,
                       0.023680172, 0.005611435,  0.001823209,  -0.000720549};

  // Initialize arrays
  for (size_t i = 0; i < nRows; i++) {
    size_t offset = i * nCols;

    for (int k = 0; k < nCols; k++) {
      ptr_prev[offset + k] = 0.0f;
      ptr_next[offset + k] = 0.0f;
      // pre-compute squared value of sample wave velocity v*v (v = 1500 m/s)
      ptr_vel[offset + k] = (1500.0f * 1500.0f);
    }
  }
  // Add a source to initial wavefield as an initial condition
  for (int s = 11; s >= 0; s--) {
    for (int i = nRows / 2 - s; i < nRows / 2 + s; i++) {
      size_t offset = i * nCols;
      for (int k = nCols / 2 - s; k < nCols / 2 + s; k++) {
        ptr_prev[offset + k] = wavelet[s];
      }
    }
  }
}

/*
 * Host-Code
 * Utility function to print device info
 */
void printTargetInfo(queue& q) {
  auto device = q.get_device();
  auto maxBlockSize =
      device.get_info<info::device::max_work_group_size>();

  auto maxEUCount =
      device.get_info<info::device::max_compute_units>();

  std::cout << " Running on " << device.get_info<info::device::name>()
            << "\n";
  std::cout << " The Device Max Work Group Size is : " << maxBlockSize
            << "\n";
  std::cout << " The Device Max EUCount is : " << maxEUCount << "\n";
}

/*
 * Host-Code
 * Utility function to calculate L2-norm between resulting buffer and reference
 * buffer
 */
bool within_epsilon(float* output, float* reference, const size_t dimx,
                    const size_t dimy, const unsigned int radius,
                    const float delta = 0.01f) {
  //FILE *fp;
  //file_error = fopen_s(&fp, "./error_diff.txt", "w");
  //if (!file_error) fp = stderr;
  //FILE* fp = fopen("./error_diff.txt", "w");
  //if (!fp) fp = stderr;
  std::ofstream errFile;
  errFile.open("error_diff.txt");

  bool error = false;
  double norm2 = 0;

  for (size_t iy = 0; iy < dimy; iy++) {
    for (size_t ix = 0; ix < dimx; ix++) {
      if (ix >= radius && ix < (dimx - radius) && iy >= radius &&
          iy < (dimy - radius)) {
        float difference = fabsf(*reference - *output);
        norm2 += difference * difference;
        if (difference > delta) {
          error = true;
          errFile<<" ERROR: "<<ix<<", "<<iy<<"   "<<*output<<"   instead of "<<
                   *reference<<"  (|e|="<<difference<<")" <<"\n";
        }
      }

      ++output;
      ++reference;
    }
  }

  //if (fp != stderr) fclose(fp);
  errFile.close();
  norm2 = sqrt(norm2);
  if (error) printf("error (Euclidean norm): %.9e\n", norm2);
  return error;
}

/*
 * Host-Code
 * CPU implementation for wavefield modeling
 * Updates wavefield for the number of iterations given in nIteratons parameter
 */
void iso_2dfd_iteration_cpu(float* next, float* prev, float* vel,
                            const float dtDIVdxy, int nRows, int nCols,
                            int nIterations) {
  float* swap;
  float value = 0.0;
  int gid = 0;
  for (unsigned int k = 0; k < nIterations; k += 1) {
    for (unsigned int i = 1; i < nRows - HALF_LENGTH; i += 1) {
      for (unsigned int j = 1; j < nCols - HALF_LENGTH; j += 1) {
        value = 0.0;

        // Stencil code to update grid
        gid = j + (i * nCols);
        value = 0.0;
        value += prev[gid + 1] - 2.0 * prev[gid] + prev[gid - 1];
        value += prev[gid + nCols] - 2.0 * prev[gid] + prev[gid - nCols];
        value *= dtDIVdxy * vel[gid];
        next[gid] = 2.0f * prev[gid] - next[gid] + value;
      }
    }

    // Swap arrays
    swap = next;
    next = prev;
    prev = swap;
  }
}

/*
 * Device-Code - GPU
 * SYCL implementation for single iteration of iso2dfd kernel
 *
 * Range kernel is used to spawn work-items in x, y dimension
 *
 */
void iso_2dfd_iteration_global(id<2> it, float* next, float* prev,
                               float* vel, const float dtDIVdxy, int nRows,
                               int nCols) {
  float value = 0.0;

  // Compute global id
  // We can use the get.global.id() function of the item variable
  //   to compute global id. The 2D array is laid out in memory in row major
  //   order.
  size_t gidRow = it.get(0);
  size_t gidCol = it.get(1);
  size_t gid = (gidRow)*nCols + gidCol;

  // Computation to solve wave equation in 2D
  // First check if gid is inside the effective grid (not in halo)
  if ((gidCol >= HALF_LENGTH && gidCol < nCols - HALF_LENGTH) &&
      (gidRow >= HALF_LENGTH && gidRow < nRows - HALF_LENGTH)) {
    // Stencil code to update grid point at position given by global id (gid)
    // New time step for grid point is computed based on the values of the
    //    the immediate neighbors in both the horizontal and vertical
    //    directions, as well as the value of grid point at a previous time step
    value = 0.0;
    value += prev[gid + 1] - 2.0 * prev[gid] + prev[gid - 1];
    value += prev[gid + nCols] - 2.0 * prev[gid] + prev[gid - nCols];
    value *= dtDIVdxy * vel[gid];
    next[gid] = 2.0f * prev[gid] - next[gid] + value;
  }
}

int main(int argc, char* argv[]) {
  // Arrays used to update the wavefield
  float* prev_base;
  float* next_base;
  float* next_cpu;
  // Array to store wave velocity
  float* vel_base;

  bool error = false;

  size_t nRows, nCols;
  unsigned int nIterations;

  // Read parameters
  try {
    nRows = std::stoi(argv[1]);
    nCols = std::stoi(argv[2]);
    nIterations = std::stoi(argv[3]);
  }

  catch (...) {
    usage(argv[0]);
    return 1;
  }

  // Compute the total size of grid
  size_t nsize = nRows * nCols;

  // Allocate arrays to hold wavefield and velocity
  prev_base = new float[nsize];
  next_base = new float[nsize];
  next_cpu = new float[nsize];
  vel_base = new float[nsize];

  // Compute constant value (delta t)^2 (delta x)^2. To be used in wavefield
  // update
  float dtDIVdxy = (DT * DT) / (DXY * DXY);

  // Initialize arrays and introduce initial conditions (source)
  initialize(prev_base, next_base, vel_base, nRows, nCols);

  std::cout << "Grid Sizes: " << nRows << " " << nCols << "\n";
  std::cout << "Iterations: " << nIterations << "\n";
  std::cout << "\n";

  // Define device selector as 'default'
  default_selector device_selector;

  // exception handler
  /*
    The exception_list parameter is an iterable list of std::exception_ptr
    objects. But those pointers are not always directly readable. So, we rethrow
    the pointer, catch it,  and then we have the exception itself. Note:
    depending upon the operation there may be several exceptions.
  */
  auto exception_handler = [](exception_list exceptionList) {
    for (std::exception_ptr const& e : exceptionList) {
      try {
        std::rethrow_exception(e);
      } catch (exception const& e) {
        std::terminate();
      }
    }
  };

  // Create a device queue using DPC++ class queue
  queue q(device_selector, exception_handler);

  std::cout << "Computing wavefield in device .." << "\n";
  // Display info about device
  printTargetInfo(q);

  // Start timer
  auto start = std::chrono::steady_clock::now();

  {  // Begin buffer scope
    // Create buffers using DPC++ class buffer
    buffer<float, 1> b_next(next_base, range<1>{nsize});
    buffer<float, 1> b_prev(prev_base, range<1>{nsize});
    buffer<float, 1> b_vel(vel_base, range<1>{nsize});

    // Iterate over time steps
    for (unsigned int k = 0; k < nIterations; k += 1) {
      // Submit command group for execution
      q.submit([&](auto &h) {
        // Create accessors
        auto next = b_next.get_access<access::mode::read_write>(h);
        auto prev = b_prev.get_access<access::mode::read_write>(h);
        auto vel = b_vel.get_access<access::mode::read>(h);

        // Define local and global range
        auto global_range = range<2>(nRows, nCols);

        // Send a DPC++ kernel (lambda) for parallel execution
        // The function that executes a single iteration is called
        // "iso_2dfd_iteration_global"
        //    alternating the 'next' and 'prev' parameters which effectively
        //    swaps their content at every iteration.
        if (k % 2 == 0)
          h.parallel_for(global_range, [=](id<2> it) {
                iso_2dfd_iteration_global(it, next.get_pointer(),
                                          prev.get_pointer(), vel.get_pointer(),
                                          dtDIVdxy, nRows, nCols);
              });
        else
          h.parallel_for(global_range, [=](id<2> it) {
                iso_2dfd_iteration_global(it, prev.get_pointer(),
                                          next.get_pointer(), vel.get_pointer(),
                                          dtDIVdxy, nRows, nCols);
              });
      });

    }  // end for

  }  // buffer scope

  // Wait for commands to complete. Enforce synchronization on the command queue
  q.wait_and_throw();

  // Compute and display time used by device
  auto end = std::chrono::steady_clock::now();
  auto time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Kernel time: " << time << " ms" << "\n";
  std::cout << "\n";

  // Output final wavefield (computed by device) to binary file
  std::ofstream outFile;
  outFile.open("wavefield_snapshot.bin", std::ios::out | std::ios::binary);
  outFile.write(reinterpret_cast<char*>(next_base), nsize * sizeof(float));
  outFile.close();

  // Compute wavefield on CPU (for validation)
  
  std::cout << "Computing wavefield in CPU .." << "\n";
  // Re-initialize arrays
  initialize(prev_base, next_cpu, vel_base, nRows, nCols);

  // Compute wavefield on CPU
  // Start timer for CPU
  start = std::chrono::steady_clock::now();
  iso_2dfd_iteration_cpu(next_cpu, prev_base, vel_base, dtDIVdxy, nRows, nCols,
                         nIterations);

  // Compute and display time used by CPU
  end = std::chrono::steady_clock::now();
  time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "CPU time: " << time << " ms" << "\n";
  std::cout << "\n";

  // Compute error (difference between final wavefields computed in device and
  // CPU)
  error = within_epsilon(next_base, next_cpu, nRows, nCols, HALF_LENGTH, 0.1f);

  // If error greater than threshold (last parameter in error function), report
  if (error)
    std::cout << "Final wavefields from device and CPU are different: Error "
              << "\n";
  else
    std::cout << "Final wavefields from device and CPU are equivalent: Success"
              << "\n";

  // Output final wavefield (computed by CPU) to binary file
  outFile.open("wavefield_snapshot_cpu.bin", std::ios::out | std::ios::binary);
  outFile.write(reinterpret_cast<char*>(next_cpu), nsize * sizeof(float));
  outFile.close();

  std::cout << "Final wavefields (from device and CPU) written to disk"
            << "\n";
  std::cout << "Finished.  " << "\n";

  // Cleanup
  delete[] prev_base;
  delete[] next_base;
  delete[] vel_base;

  return error ? 1 : 0;
}
