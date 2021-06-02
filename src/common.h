// This file is part of dvs-panotracking.
//
// Copyright (C) 2017 Christian Reinbacher <reinbacher at icg dot tugraz dot at>
// Institute for Computer Graphics and Vision, Graz University of Technology
// https://www.tugraz.at/institute/icg/teams/team-pock/
//
// dvs-panotracking is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or any later version.
//
// dvs-panotracking is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef COMMON_H
#define COMMON_H
#include "iu/iucore.h"
#include "event.h"
#include <string>
#include <vector>
#include <Eigen/Dense>

#define GPU_BLOCK_SIZE 16
#define TIME_CONSTANT 1e-6f

typedef Eigen::Matrix<float, 3, 3, Eigen::RowMajor> Matrix3fr;

// IO functions
void loadEvents(std::vector<Event> &events, const Matrix3fr &K, float radial, std::string filename);
void loadEvents(std::vector<Event> &events, std::string filename);
void saveEvents(std::string filename, std::vector<Event> &events);
void saveState(std::string filename, const iu::ImageGpu_32f_C1 *mat, bool as_png, bool as_npy, bool as_exr);
void saveState(std::string filename, const iu::ImageGpu_8u_C4 *mat);
// helper function
bool undistortPoint(Event &event, const Matrix3fr &K, float radial, int camera_width = 128, int camera_height = 128);

// Define this to turn on error checking
#define CUDA_ERROR_CHECK

#define CudaSafeCall(err) __cudaSafeCall(err, __FILE__, __LINE__)
#define CudaCheckError() __cudaCheckError(__FILE__, __LINE__)

inline void __cudaSafeCall(cudaError err, const char *file, const int line)
{
#ifdef CUDA_ERROR_CHECK
    if (cudaSuccess != err)
    {
        fprintf(stderr, "cudaSafeCall() failed at %s:%i : %s\n",
                file, line, cudaGetErrorString(err));
        exit(-1);
    }
#endif

    return;
}

inline void __cudaCheckError(const char *file, const int line)
{
#ifdef CUDA_ERROR_CHECK
    //cudaDeviceSynchronize();
    cudaError err = cudaGetLastError();
    if (cudaSuccess != err)
    {
        fprintf(stderr, "cudaCheckError() failed at %s:%i : %s\n",
                file, line, cudaGetErrorString(err));
        exit(-1);
    }
#endif
}
#endif // COMMON_H
