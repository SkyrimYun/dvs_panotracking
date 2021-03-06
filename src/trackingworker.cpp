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

#include <QMutexLocker>
#include <fstream>
#include "trackingworker.h"
#include "common.cuh"
#include "common.h"
#include "direct.cuh"
#include "cnpy.h"
#include "iu/iuio.h"
#include "iu/iumath.h"
#include "eigenhelpers.h"

TrackingWorker::TrackingWorker(const Parameters &cam_parameters, int device_number, float upscale)
{
    device_number_ = device_number;
    CudaSafeCall(cudaSetDevice(device_number_));
    width_ = cam_parameters.camera_width;
    height_ = cam_parameters.camera_height;
    output_ = new iu::ImageGpu_32f_C1(cam_parameters.output_size_x, cam_parameters.output_size_y);
    output_color_ = new iu::ImageGpu_8u_C4(cam_parameters.output_size_x, cam_parameters.output_size_y);
    occurences_ = new iu::ImageGpu_32f_C1(cam_parameters.output_size_x, cam_parameters.output_size_y);
    normalization_ = new iu::ImageGpu_32f_C1(cam_parameters.output_size_x, cam_parameters.output_size_y);

    iu::math::fill(*occurences_, 0.f);
    iu::math::fill(*normalization_, 1.f);
    iu::math::fill(*output_, 0.f);

    events_per_image_ = 1500;
    iterations_ = 10;
    image_skip_ = 5;

    camera_parameters_ = cam_parameters;
    upscale_ = upscale;
    tracking_quality_ = 1;
    image_id_ = 0;

    cuda::setCameraMatrices(camera_parameters_.K_cam, camera_parameters_.K_caminv, camera_parameters_.px, camera_parameters_.py, upscale_);

    events_cpu_ = NULL;
    events_gpu_ = NULL;
    image_gradients_cpu_ = NULL;
    image_gradients_gpu_ = NULL;

    pose_.setZero();
    old_pose_ = pose_;

    R_sphere_ << 0, 0, 1,
        1, 0, 0,
        0, 1, 0;

    lambda_ = 100.f;
    lambda_a_ = 2.f;
    lambda_b_ = 10.f;
    alpha_ = 0.4f;

    show_camera_pose_ = true;
    show_events_ = true;

    // yunfan
    getUndistortMap();
}

void TrackingWorker::addEvents(std::vector<Event> &events)
{
    if (all_events_.size() > 0)
    {
        all_events_.clear();
    }
    QMutexLocker lock(&mutex_events_);
    for (int i = 0; i < events.size(); i++)
    {
        events_.push(events[i]);
        all_events_.push_back(events[i]);
    }
}

void TrackingWorker::saveEvents(std::string filename)
{
    ::saveEvents(filename, all_events_);
}

void TrackingWorker::run()
{
    CudaSafeCall(cudaSetDevice(device_number_));
    if(reset_pose_)
    {
        iu::math::fill(*occurences_, 0);
        iu::math::fill(*normalization_, 1.f);
        pose_.setZero();
        old_pose_.setZero();
    }
    all_events_.clear();
    running_ = true;
    tracking_quality_ = 1;
    image_id_ = 0;
    //int event_id = 0;
    while (running_)
    {
        mutex_events_.lock();
        bool image_available = events_.size() > events_per_image_;
        mutex_events_.unlock();
        if (image_available)
        {
            mutex_events_.lock();
            std::vector<Event> temp_events;
            for (int i = 0; i < events_per_image_; i++)
            {
                temp_events.push_back(events_.front());
                events_.pop();
            }
            mutex_events_.unlock();

            //yunfan
            float t_packet_begin = temp_events.front().t;
            float t_packet_end = temp_events.back().t;
            packet_t_ = t_packet_begin + 0.5 * (t_packet_end - t_packet_begin);

            track(temp_events);
        }
        else
            msleep(1);
    }
}

void TrackingWorker::stop()
{
    running_ = false;
    clearEvents();
    if(reset_pose_)
    {
        iu::math::fill(*occurences_, 0);
        iu::math::fill(*normalization_, 1.f);
        pose_.setZero();
        old_pose_.setZero();
    }
    all_events_.clear();
    tracking_quality_ = 1;
    image_id_ = 0;
}

void TrackingWorker::updateScale(double value)
{
    upscale_ = value;
    cuda::setCameraMatrices(camera_parameters_.K_cam, camera_parameters_.K_caminv, camera_parameters_.px, camera_parameters_.py, upscale_);
}

void TrackingWorker::track(std::vector<Event> &events)
{
    static iu::IuCudaTimer timer;
    timer.start();

    double time_map;
    double time_track;

    // Keep CPU<->GPU interface memory up-to-date
    if (!events_cpu_ || events_cpu_->numel() != events.size())
        events_cpu_ = new iu::LinearHostMemory_32f_C2(events.size());
    if (!events_gpu_ || events_gpu_->numel() != events.size())
        events_gpu_ = new iu::LinearDeviceMemory_32f_C2(events.size());
    if (!image_gradients_cpu_ || image_gradients_cpu_->numel() != events.size())
        image_gradients_cpu_ = new iu::LinearHostMemory_32f_C4(events.size());
    if (!image_gradients_gpu_ || image_gradients_gpu_->numel() != events.size())
        image_gradients_gpu_ = new iu::LinearDeviceMemory_32f_C4(events.size());

    for (int i = 0; i < events.size(); i++)
    {
        // yunfan
        if (::undistortPoint(events[i], undistorted, camera_parameters_.camera_width, camera_parameters_.camera_height))
            *events_cpu_->data(i) = make_float2(events[i].x_undist, events[i].y_undist);
    }
    iu::copy(events_cpu_, events_gpu_);

    if (image_id_ > 10)
    { // First few poses are crap anyhow, since there is no map.
        //timer.start();
        bool successfull = updatePose();
        time_track = timer.elapsed();

        // yunfan
        static std::ofstream pose_output_(camera_parameters_.pose_output_dir + "/output_pose/estimated_pose_rpg.txt",std::ios::trunc);
        double rad = pose_.norm();
        Eigen::AngleAxisd aa(rad, Eigen::Vector3d(pose_[1], pose_[2], pose_[0]) / rad);
        Eigen::Quaterniond q_eigen(aa);
        pose_output_ << packet_t_ << " 0 0 0 " << q_eigen.x() << " " << q_eigen.y() << " " << q_eigen.z() << " " << q_eigen.w() << std::endl;

        if (successfull && tracking_quality_ > 0.25f)
        { // first few events often contain only noise. Update map only when tracking is good (arbitrary th).
            timer.start();
            cuda::updateMap(output_, occurences_, normalization_, events_gpu_, make_float3(pose_(0), pose_(1), pose_(2)), make_float3(old_pose_(0), old_pose_(1), old_pose_(2)), width_, height_);
            time_map = timer.elapsed();
        }
    }
    else
    {
        cuda::updateMap(output_, occurences_, normalization_, events_gpu_, make_float3(pose_(0), pose_(1), pose_(2)), make_float3(old_pose_(0), old_pose_(1), old_pose_(2)), width_, height_);
    }
    image_id_++;
    if (image_skip_ > 0 && (image_id_ % image_skip_) == 0)
    {
        // yunfan
        end_t = clock();

        emit update_info(tr("Track: %1s Map: %2ms. Quality: %3").arg(double(end_t - start_t) / CLOCKS_PER_SEC).arg(time_map).arg(tracking_quality_), 0);
        cuda::createOutput(output_color_, output_, show_events_ ? events_gpu_ : NULL, make_float3(pose_(0), pose_(1), pose_(2)), width_, height_, show_camera_pose_ ? tracking_quality_ : -1.f);
        emit update_output(output_color_);
    }
}

Matrix3fr TrackingWorker::rodrigues(Eigen::Vector3f in)
{
    float theta = in.norm();
    if (theta < 1e-8f)
    {
        return Matrix3fr::Identity();
    }
    Eigen::Vector3f omega = in / theta;
    float alpha = cos(theta);
    float beta = sin(theta);
    float gamma = 1 - alpha;

    // R = eye(3)*alpha + crossmat(omega)*beta + omega*omega'*gamma
    return Matrix3fr::Identity() * alpha + crossmat(omega) * beta + omega * omega.transpose() * gamma;
}

Matrix3fr TrackingWorker::crossmat(Eigen::Vector3f t)
{
    Matrix3fr t_hat;
    t_hat << 0, -t(2), t(1),
        t(2), 0, -t(0),
        -t(1), t(0), 0;
    return t_hat;
}

bool TrackingWorker::updatePose()
{

    // Pre-calculate stuff which doesn't change between iterations
    Eigen::Map<Eigen::Matrix2Xf> events((float *)events_cpu_->data(), 2, events_cpu_->numel());
    Eigen::Matrix3Xf points(3, events.cols());
    points.topLeftCorner(events.rows(), events.cols()) = events;
    points.bottomRows<1>().setOnes();
    points = R_sphere_ * camera_parameters_.K_caminv * points;
    // yunfan
    //points = camera_parameters_.K_caminv * points;

    Eigen::Matrix3Xf X_hat(3, events.cols());
    Eigen::RowVectorXf X_hat_norm(events.cols());
    Eigen::MatrixX3f J(events.cols(), 3);
    Eigen::MatrixX3f dG_dgsi(9, 3);
    Eigen::Matrix3Xf dg_dG(3, 9);
    Eigen::Matrix3f JtJ(3, 3);
    Eigen::Matrix2Xf dPI_dg(2, 3);
    Eigen::Map<Eigen::Matrix4Xf> dM_dx((float *)image_gradients_cpu_->data(), 4, events_cpu_->numel());
    Eigen::Map<Eigen::VectorXf, 0, Eigen::Stride<0, 4> > M(&image_gradients_cpu_->data(0)->z, events_cpu_->numel());

    old_pose_ = pose_;
    Eigen::Vector3f old_pose = pose_;
    Eigen::Vector3f init_pose = pose_;
    Eigen::Vector3f accel_pose = pose_;
    for (int iteration = 0; iteration < iterations_; iteration++)
    {
        Eigen::Matrix3f R = rodrigues(accel_pose);
        X_hat = R * points;
        X_hat_norm = X_hat.array().square().colwise().sum();
        // get image gradients from GPU -> move to CPU
        cuda::getGradients(image_gradients_gpu_, output_, events_gpu_, make_float3(accel_pose(0), accel_pose(1), accel_pose(2)));
        iu::copy(image_gradients_gpu_, image_gradients_cpu_);
        dG_dgsi << crossmat(-R.row(0)), crossmat(-R.row(1)), crossmat(-R.row(2));
        JtJ.setZero();
        for (int id = 0; id < events.cols(); id++)
        {
            dg_dG << X_hat(0, id) * Eigen::Matrix3f::Identity(),
                X_hat(1, id) * Eigen::Matrix3f::Identity(),
                X_hat(2, id) * Eigen::Matrix3f::Identity();
            dPI_dg(0, 0) = -camera_parameters_.px * X_hat(1, id) / X_hat_norm(id) / M_PI;
            dPI_dg(0, 1) = camera_parameters_.px * X_hat(0, id) / X_hat_norm(id) / M_PI;
            dPI_dg(0, 2) = 0.f;
            dPI_dg(1, 0) = -camera_parameters_.py * X_hat(0, id) * X_hat(2, id) / pow(X_hat_norm(id), 3.f / 2.f) / (camera_parameters_.px / camera_parameters_.py);
            dPI_dg(1, 1) = -camera_parameters_.py * X_hat(1, id) * X_hat(2, id) / pow(X_hat_norm(id), 3.f / 2.f) / (camera_parameters_.px / camera_parameters_.py);
            dPI_dg(1, 2) = camera_parameters_.py / X_hat_norm(id) / (camera_parameters_.px / camera_parameters_.py);
            dPI_dg *= upscale_;
            J.row(id) = dM_dx.block<2, 1>(0, id).transpose() * dPI_dg * dg_dG * dG_dgsi;
            JtJ += J.row(id).transpose() * J.row(id);
        }
        // Gauss-Newton with prox
        float alpha = 1.f;
        old_pose = pose_;
        pose_ = accel_pose - (JtJ + alpha * JtJ.diagonal().asDiagonal().toDenseMatrix()).inverse() * ((J.transpose() * -M) - alpha * (accel_pose - init_pose));
        accel_pose = pose_ + alpha_ * (pose_ - old_pose);
    }
    tracking_quality_ = std::min(M.sum() / M.size() * upscale_, 1.f);
    return true;
}

void TrackingWorker::saveCurrentState(std::string filename)
{
    saveState(filename, output_color_);
}

void TrackingWorker::clearEvents()
{
    QMutexLocker lock(&mutex_events_);
    std::queue<Event> empty;
    std::swap(events_, empty);
}

void TrackingWorker::getUndistortMap()
{
    undistorted = std::vector<int>(width_ * height_, -1);

    float fx = camera_parameters_.K_cam(0, 0);
    float fy = camera_parameters_.K_cam(1, 1);
    float cx = camera_parameters_.K_cam(0, 2);
    float cy = camera_parameters_.K_cam(1, 2);

    float k1 = camera_parameters_.distort.k1;
    float k2 = camera_parameters_.distort.k2;
    float p1 = camera_parameters_.distort.p1;
    float p2 = camera_parameters_.distort.p2;


    for (int v = 0; v < height_; v++)
    {
        for (int u = 0; u < width_; u++)
        {
            float x = (u - cx) / fx, y = (v - cy) / fy;
            float r = sqrt(x * x + y * y);
            float x_distorted = x * (1 + k1 * r * r + k2 * r * r * r * r) + 2 * p1 * x * y + p2 * (r * r + 2 * x * x);
            float y_distorted = y * (1 + k1 * r * r + k2 * r * r * r * r) + p1 * (r * r + 2 * y * y) + 2 * p2 * x * y;
            float u_distorted = fx * x_distorted + cx;
            float v_distorted = fy * y_distorted + cy;

            int idx_distort = (int)v_distorted * width_ + (int)u_distorted;
            int idx_undistort = (int)v * width_ + (int)u;
            if (u_distorted >= 0 && v_distorted >= 0 && u_distorted < width_ && v_distorted < height_)
            {
                undistorted[idx_distort] = idx_undistort;
            }
        }
    }
}