/*
 * Copyright [2015] [Ke Sun]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <imu_vn_100/imu_vn_100.h>

namespace imu_vn_100 {

// LESS HACK IS STILL HACK
ImuVn100* imu_vn_100_ptr;

using sensor_msgs::FluidPressure;
using sensor_msgs::Imu;
using sensor_msgs::MagneticField;
using sensor_msgs::Temperature;

void RosVector3FromVnVector3(geometry_msgs::Vector3& ros_vec3,
                             const VnVector3& vn_vec3,
                             bool enu_output = false,
                             bool reverse_z = false);
void RosQuaternionFromVnQuaternion(geometry_msgs::Quaternion& ros_quat,
                                   const VnQuaternion& vn_quat,
                                   bool enu_output = false);
void FillImuMessage(sensor_msgs::Imu& imu_msg,
                    const VnDeviceCompositeData& data,
                    bool binary_output,
                    bool enu_output = false,
                    bool reverse_gravity = false);

tf2::Quaternion ImuNedToEnu(double x, double y, double z, double w);

void AsyncListener(void* sender, VnDeviceCompositeData* data) {
  imu_vn_100_ptr->PublishData(*data);
}

constexpr int ImuVn100::kBaseImuRate;
constexpr int ImuVn100::kDefaultImuRate;
constexpr int ImuVn100::kDefaultSyncOutRate;

bool ImuVn100::SyncInfo::Update(const unsigned sync_count,
                                const ros::Time& sync_time,
                                const ros::Duration& time_since_sync_in) {
  if (rate <= 0) return false;

  bool ret = false;
  if (count != sync_count) {
    boost::lock_guard<boost::mutex> guard(mtx_);
    count = sync_count;
    time = sync_time - time_since_sync_in;
    cv_.notify_all();
    ret = true;
  }
  return ret;
}

bool ImuVn100::SyncInfo::SyncEnabled() const { return rate > 0; }

void ImuVn100::SyncInfo::FixSyncRate() {
  // Check the sync out rate
  if (SyncEnabled()) {
    if (ImuVn100::kBaseImuRate % rate != 0) {
      rate = ImuVn100::kBaseImuRate / (ImuVn100::kBaseImuRate / rate);
      ROS_INFO("Set SYNC_OUT_RATE to %d", rate);
    }
    skip_count =
        (std::floor(ImuVn100::kBaseImuRate / static_cast<double>(rate) +
                    0.5f)) -
        1;

    if (pulse_width_us > 10000) {
      ROS_INFO("Sync out pulse with is over 10ms. Reset to 1ms");
      pulse_width_us = 1000;
    }
    rate_double = rate;
  }

  ROS_INFO("Sync out rate: %d", rate);
}

ImuVn100::ImuVn100(const ros::NodeHandle& pnh)
    : pnh_(pnh),
      port_(std::string("/dev/ttyUSB0")),
      baudrate_(921600),
      frame_id_(std::string("imu")) {
  Initialize();
  imu_vn_100_ptr = this;
}

ImuVn100::~ImuVn100() { Disconnect(); }

void ImuVn100::FixImuRate() {
  if (imu_rate_ <= 0) {
    ROS_WARN("Imu rate %d is < 0. Set to %d", imu_rate_, kDefaultImuRate);
    imu_rate_ = kDefaultImuRate;
  }

  if (kBaseImuRate % imu_rate_ != 0) {
    int imu_rate_old = imu_rate_;
    imu_rate_ = kBaseImuRate / (kBaseImuRate / imu_rate_old);
    ROS_WARN("Imu rate %d cannot evenly decimate base rate %d, reset to %d",
             imu_rate_old, kBaseImuRate, imu_rate_);
  }
}

void ImuVn100::LoadParameters() {
  pnh_.param<std::string>("port", port_, std::string("/dev/ttyUSB0"));
  pnh_.param<std::string>("frame_id", frame_id_, pnh_.getNamespace());
  pnh_.param("baudrate", baudrate_, 115200);
  pnh_.param("imu_rate", imu_rate_, kDefaultImuRate);

  pnh_.param("enable_mag", enable_mag_, true);
  pnh_.param("enable_pres", enable_pres_, true);
  pnh_.param("enable_temp", enable_temp_, true);

  pnh_.param("sync_rate", sync_info_.rate, kDefaultSyncOutRate);
  pnh_.param("sync_pulse_width_us", sync_info_.pulse_width_us, 1000);

  pnh_.param("binary_output", binary_output_, true);
  pnh_.param("binary_async_mode", binary_async_mode, BINARY_ASYNC_MODE_SERIAL_1);

  pnh_.param("enu_output", enu_output, false);
  pnh_.param("reverse_gravity", reverse_gravity, false);

  pnh_.param("vpe_enable", vpe_enable_, true);

  pnh_.param("vpe_heading_mode", vpe_heading_mode_, 1);
  pnh_.param("vpe_filtering_mode", vpe_filtering_mode_, 1);
  pnh_.param("vpe_tuning_mode", vpe_tuning_mode_, 1);

  pnh_.param("vpe_mag_tuning_base_tuning_x",  vpe_mag_base_tuning_.c0, 10.0);
  pnh_.param("vpe_mag_tuning_base_tuning_y",  vpe_mag_base_tuning_.c1, 10.0);
  pnh_.param("vpe_mag_tuning_base_tuning_z",  vpe_mag_base_tuning_.c2, 10.0);
  pnh_.param("vpe_mag_tuning_adaptive_tuning_x", vpe_mag_adaptive_tuning_.c0, 10.0);
  pnh_.param("vpe_mag_tuning_adaptive_tuning_y", vpe_mag_adaptive_tuning_.c1, 10.0);
  pnh_.param("vpe_mag_tuning_adaptive_tuning_z", vpe_mag_adaptive_tuning_.c2, 10.0);
  pnh_.param("vpe_mag_tuning_adaptive_filtering_x", vpe_mag_adaptive_filtering_.c0, 10.0);
  pnh_.param("vpe_mag_tuning_adaptive_filtering_y", vpe_mag_adaptive_filtering_.c1, 10.0);
  pnh_.param("vpe_mag_tuning_adaptive_filtering_z", vpe_mag_adaptive_filtering_.c2, 10.0);

  pnh_.param("vpe_accel_tuning_base_tuning_x", vpe_accel_base_tuning_.c0, 5.0);
  pnh_.param("vpe_accel_tuning_base_tuning_y", vpe_accel_base_tuning_.c1, 5.0);
  pnh_.param("vpe_accel_tuning_base_tuning_z", vpe_accel_base_tuning_.c2, 5.0);
  pnh_.param("vpe_accel_tuning_adaptive_tuning_x", vpe_accel_adaptive_tuning_.c0, 3.0);
  pnh_.param("vpe_accel_tuning_adaptive_tuning_y", vpe_accel_adaptive_tuning_.c1, 3.0);
  pnh_.param("vpe_accel_tuning_adaptive_tuning_z", vpe_accel_adaptive_tuning_.c2, 3.0);
  pnh_.param("vpe_accel_tuning_adaptive_filtering_x", vpe_accel_adaptive_filtering_.c0, 4.0);
  pnh_.param("vpe_accel_tuning_adaptive_filtering_y", vpe_accel_adaptive_filtering_.c1, 4.0);
  pnh_.param("vpe_accel_tuning_adaptive_filtering_z", vpe_accel_adaptive_filtering_.c2, 4.0);

  FixImuRate();
  sync_info_.FixSyncRate();
}

void ImuVn100::CreateDiagnosedPublishers() {
  imu_rate_double_ = imu_rate_;
  pd_imu_.Create<Imu>(pnh_, "imu", updater_, imu_rate_double_);
  if (enable_mag_) {
    pd_mag_.Create<MagneticField>(pnh_, "magnetic_field", updater_,
                                  imu_rate_double_);
  }
  if (enable_pres_) {
    pd_pres_.Create<FluidPressure>(pnh_, "fluid_pressure", updater_,
                                   imu_rate_double_);
  }
  if (enable_temp_) {
    pd_temp_.Create<Temperature>(pnh_, "temperature", updater_,
                                 imu_rate_double_);
  }
}

void ImuVn100::Initialize() {
  LoadParameters();

  ROS_DEBUG("Connecting to device");
  VnEnsure(vn100_connect(&imu_, port_.c_str(), 115200));
  ros::Duration(0.5).sleep();
  ROS_INFO("Connected to device at %s", port_.c_str());

  unsigned int old_baudrate;
  VnEnsure(vn100_getSerialBaudRate(&imu_, &old_baudrate));
  ROS_INFO("Default serial baudrate: %u", old_baudrate);

  ROS_INFO("Set serial baudrate to %d", baudrate_);
  VnEnsure(vn100_setSerialBaudRate(&imu_, baudrate_, true));

  ROS_DEBUG("Disconnecting the device");
  vn100_disconnect(&imu_);
  ros::Duration(0.5).sleep();

  ROS_DEBUG("Reconnecting to device");
  VnEnsure(vn100_connect(&imu_, port_.c_str(), baudrate_));
  ros::Duration(0.5).sleep();
  ROS_INFO("Connected to device at %s", port_.c_str());

  VnEnsure(vn100_getSerialBaudRate(&imu_, &old_baudrate));
  ROS_INFO("New serial baudrate: %u", old_baudrate);

  // Idle the device for intialization
  VnEnsure(vn100_pauseAsyncOutputs(&imu_, true));

  ROS_INFO("Fetching device info.");
  char model_number_buffer[30] = {0};
  int hardware_revision = 0;
  char serial_number_buffer[30] = {0};
  char firmware_version_buffer[30] = {0};

  VnEnsure(vn100_getModelNumber(&imu_, model_number_buffer, 30));
  ROS_INFO("Model number: %s", model_number_buffer);
  VnEnsure(vn100_getHardwareRevision(&imu_, &hardware_revision));
  ROS_INFO("Hardware revision: %d", hardware_revision);
  VnEnsure(vn100_getSerialNumber(&imu_, serial_number_buffer, 30));
  ROS_INFO("Serial number: %s", serial_number_buffer);
  VnEnsure(vn100_getFirmwareVersion(&imu_, firmware_version_buffer, 30));
  ROS_INFO("Firmware version: %s", firmware_version_buffer);

  if (sync_info_.SyncEnabled()) {
    ROS_INFO("Set Synchronization Control Register (id:32).");
    VnEnsure(vn100_setSynchronizationControl(
        &imu_, SYNCINMODE_COUNT, SYNCINEDGE_RISING, 0, SYNCOUTMODE_IMU_START,
        SYNCOUTPOLARITY_POSITIVE, sync_info_.skip_count,
        sync_info_.pulse_width_us * 1000, true));

    if (!binary_output_) {
      ROS_INFO("Set Communication Protocal Control Register (id:30).");
      VnEnsure(vn100_setCommunicationProtocolControl(
          &imu_, SERIALCOUNT_SYNCOUT_COUNT, SERIALSTATUS_OFF, SPICOUNT_NONE,
          SPISTATUS_OFF, SERIALCHECKSUM_8BIT, SPICHECKSUM_8BIT, ERRORMODE_SEND,
          true));
    }

  uint8_t vpe_enable;
  uint8_t vpe_heading_mode;
  uint8_t vpe_filtering_mode;
  uint8_t vpe_tuning_mode;
  VnEnsure(vn100_getVpeControl(&imu_, &vpe_enable, &vpe_heading_mode,
                               &vpe_filtering_mode, &vpe_tuning_mode));

  ROS_INFO("Default VPE enable: %hhu", vpe_enable);
  ROS_INFO("Default VPE heading mode: %hhu", vpe_heading_mode);
  ROS_INFO("Default VPE filtering mode: %hhu", vpe_filtering_mode);
  ROS_INFO("Default VPE tuning mode: %hhu", vpe_tuning_mode);

  if (vpe_enable != vpe_enable_ ||
      vpe_heading_mode != vpe_heading_mode_ ||
      vpe_filtering_mode != vpe_filtering_mode_ ||
      vpe_tuning_mode != vpe_tuning_mode_) {
      vpe_enable = vpe_enable_;
      vpe_heading_mode = vpe_heading_mode_;
      vpe_filtering_mode = vpe_filtering_mode_;
      vpe_tuning_mode = vpe_tuning_mode_;

      ROS_INFO("Setting VPE enable: %hhu", vpe_enable);
      ROS_INFO("Setting VPE heading mode: %hhu", vpe_heading_mode);
      ROS_INFO("Setting VPE filtering mode: %hhu", vpe_filtering_mode);
      ROS_INFO("Setting VPE tuning mode: %hhu", vpe_tuning_mode);

      VnEnsure(vn100_setVpeControl(
        &imu_,
        vpe_enable,
        vpe_heading_mode,
        vpe_filtering_mode,
        vpe_tuning_mode,
        true));
  }

  if (vpe_enable_) {
      ROS_INFO("Setting VPE MagnetometerBasicTuning BaseTuning (%f, %f, %f)",
         vpe_mag_base_tuning_.c0,
         vpe_mag_base_tuning_.c1,
         vpe_mag_base_tuning_.c2);
      ROS_INFO("Setting VPE MagnetometerBasicTuning AdaptiveTuning (%f, %f, %f)",
        vpe_mag_adaptive_tuning_.c0,
        vpe_mag_adaptive_tuning_.c1,
        vpe_mag_adaptive_tuning_.c2);
      ROS_INFO("Setting VPE MagnetometerBasicTuning AdaptiveFiltering (%f, %f, %f)",
        vpe_mag_adaptive_filtering_.c0,
        vpe_mag_adaptive_filtering_.c1,
        vpe_mag_adaptive_filtering_.c2);
      VnEnsure(vn100_setVpeMagnetometerBasicTuning(
        &imu_,
        vpe_mag_base_tuning_,
        vpe_mag_adaptive_tuning_,
        vpe_mag_adaptive_filtering_,
        true));

      ROS_INFO("Setting VPE AccelerometerBasicTuning BaseTuning (%f, %f, %f)",
        vpe_accel_base_tuning_.c0,
        vpe_accel_base_tuning_.c1,
        vpe_accel_base_tuning_.c2);
      ROS_INFO("Setting VPE AccelerometerBasicTuning AdaptiveTuning (%f, %f, %f)",
        vpe_accel_adaptive_tuning_.c0,
        vpe_accel_adaptive_tuning_.c1,
        vpe_accel_adaptive_tuning_.c2);
      ROS_INFO("Setting VPE AccelerometerBasicTuning AdaptiveFiltering (%f, %f, %f)",
        vpe_accel_adaptive_filtering_.c0,
        vpe_accel_adaptive_filtering_.c1,
        vpe_accel_adaptive_filtering_.c2);
      VnEnsure(vn100_setVpeAccelerometerBasicTuning(
        &imu_,
        vpe_accel_base_tuning_,
        vpe_accel_adaptive_tuning_,
        vpe_accel_adaptive_filtering_,
        true));
  }
  }

  // Set the initial heading to 0
  VnEnsure(vn100_tare(&imu_, true));

  CreateDiagnosedPublishers();

  auto hardware_id = std::string("vn100-") + std::string(model_number_buffer) +
                     std::string(serial_number_buffer);
  updater_.setHardwareID(hardware_id);
}

void ImuVn100::Stream(bool async) {
  // Pause the device first
  VnEnsure(vn100_pauseAsyncOutputs(&imu_, true));

  if (async) {
    VnEnsure(vn100_setAsynchronousDataOutputType(&imu_, VNASYNC_OFF, true));

    if (binary_output_) {
      // Set the binary output data type and data rate
      VnEnsure(vn100_setBinaryOutput1Configuration(
          &imu_, binary_async_mode, kBaseImuRate / imu_rate_,
          BG1_QTN | BG1_IMU | BG1_SYNC_IN_CNT | BG1_TIME_STARTUP,
          BG3_ACCEL | BG3_GYRO, BG5_NONE, true));
    } else {
      // Set the ASCII output data type and data rate
      // ROS_INFO("Configure the output data type and frequency (id: 6 & 7)");
      VnEnsure(vn100_setAsynchronousDataOutputType(&imu_, VNASYNC_VNIMU, true));
    }

    // Add a callback function for new data event
    VnEnsure(vn100_registerAsyncDataReceivedListener(&imu_, &AsyncListener));

    ROS_INFO("Setting IMU rate to %d", imu_rate_);
    VnEnsure(vn100_setAsynchronousDataOutputFrequency(&imu_, imu_rate_, true));
  } else {
    // Mute the stream
    ROS_DEBUG("Mute the device");
    VnEnsure(vn100_setAsynchronousDataOutputType(&imu_, VNASYNC_OFF, true));
    // Remove the callback function for new data event
    VnEnsure(vn100_unregisterAsyncDataReceivedListener(&imu_, &AsyncListener));
  }

  // Resume the device
  VnEnsure(vn100_resumeAsyncOutputs(&imu_, true));
}

void ImuVn100::Resume(bool need_reply) {
  vn100_resumeAsyncOutputs(&imu_, need_reply);
}

void ImuVn100::Idle(bool need_reply) {
  vn100_pauseAsyncOutputs(&imu_, need_reply);
}

void ImuVn100::Disconnect() {
  // TODO: why reset the device?
  vn100_reset(&imu_);
  vn100_disconnect(&imu_);
}

void ImuVn100::PublishData(const VnDeviceCompositeData& data) {
  const ros::Time t_now = ros::Time::now();

  constexpr uint64_t k1e9 = 1000000000;
  const auto time_since_sync_in =
      ros::Duration(data.timeSyncIn / k1e9, data.timeSyncIn % k1e9);

  bool updated = sync_info_.Update(data.syncInCnt, t_now, time_since_sync_in);
  if (updated) imu_timestamp_offset_ = time_since_sync_in;

  sensor_msgs::Imu imu_msg;
  imu_msg.header.stamp = t_now - imu_timestamp_offset_;
  imu_msg.header.frame_id = frame_id_;

  FillImuMessage(imu_msg, data, binary_output_, enu_output, reverse_gravity);
  pd_imu_.Publish(imu_msg);

  if (enable_mag_) {
    sensor_msgs::MagneticField mag_msg;
    mag_msg.header = imu_msg.header;
    RosVector3FromVnVector3(mag_msg.magnetic_field, data.magnetic);
    pd_mag_.Publish(mag_msg);
  }

  if (enable_pres_) {
    sensor_msgs::FluidPressure pres_msg;
    pres_msg.header = imu_msg.header;
    pres_msg.fluid_pressure = data.pressure;
    pd_pres_.Publish(pres_msg);
  }

  if (enable_temp_) {
    sensor_msgs::Temperature temp_msg;
    temp_msg.header = imu_msg.header;
    temp_msg.temperature = data.temperature;
    pd_temp_.Publish(temp_msg);
  }

  updater_.update();
}

void VnEnsure(const VnErrorCode& error_code) {
  if (error_code == VNERR_NO_ERROR) return;

  switch (error_code) {
    case VNERR_UNKNOWN_ERROR:
      throw std::runtime_error("VN: Unknown error");
    case VNERR_NOT_IMPLEMENTED:
      throw std::runtime_error("VN: Not implemented");
    case VNERR_TIMEOUT:
      ROS_WARN("VN: Operation time out");
      break;
    case VNERR_SENSOR_INVALID_PARAMETER:
      ROS_WARN("VN: Sensor invalid paramter");
      break;
    case VNERR_INVALID_VALUE:
      ROS_WARN("VN: Invalid value");
      break;
    case VNERR_FILE_NOT_FOUND:
      ROS_WARN("VN: File not found");
      break;
    case VNERR_NOT_CONNECTED:
      throw std::runtime_error("VN: not connected");
    case VNERR_PERMISSION_DENIED:
      throw std::runtime_error("VN: Permission denied");
    default:
      ROS_WARN("Unhandled error type");
  }
}

void RosVector3FromVnVector3(geometry_msgs::Vector3& ros_vec3,
                             const VnVector3& vn_vec3,
                             bool enu_output,
                             bool reverse_z) {
  if(enu_output) {
    ros_vec3.x = vn_vec3.c0;
    ros_vec3.y = -vn_vec3.c1;
    ros_vec3.z = -vn_vec3.c2;
  }
  else {
    ros_vec3.x = vn_vec3.c0;
    ros_vec3.y = vn_vec3.c1;
    ros_vec3.z = vn_vec3.c2;
  }

  if(reverse_z) {
    // Linear acceleration on z from VN-100 seems to be reversed
    ros_vec3.z = -ros_vec3.z;
  }
}

void RosQuaternionFromVnQuaternion(geometry_msgs::Quaternion& ros_quat,
                                   const VnQuaternion& vn_quat,
                                   bool enu_output) {
  if(enu_output) {
    tf2::Quaternion quat = ImuNedToEnu(vn_quat.x, vn_quat.y,
                                       vn_quat.z, vn_quat.w);
    ros_quat.x = quat.getX();
    ros_quat.y = quat.getY();
    ros_quat.z = quat.getZ();
    ros_quat.w = quat.getW();
  }
  else {
    ros_quat.x = vn_quat.x;
    ros_quat.y = vn_quat.y;
    ros_quat.z = vn_quat.z;
    ros_quat.w = vn_quat.w;
  }
}

void FillImuMessage(sensor_msgs::Imu& imu_msg,
                    const VnDeviceCompositeData& data,
                    bool binary_output,
                    bool enu_output,
                    bool reverse_gravity) {
  if (binary_output) {
    RosQuaternionFromVnQuaternion(imu_msg.orientation, data.quaternion, enu_output);
    // NOTE: The IMU angular velocity and linear acceleration outputs are
    // swapped. And also why are they different?
    RosVector3FromVnVector3(imu_msg.angular_velocity,
                            data.angularRate,
                            enu_output);
    RosVector3FromVnVector3(imu_msg.linear_acceleration,
                            data.acceleration,
                            enu_output,
                            reverse_gravity);
  } else {
    RosVector3FromVnVector3(imu_msg.linear_acceleration, data.acceleration, enu_output, reverse_gravity);
    RosVector3FromVnVector3(imu_msg.angular_velocity, data.angularRate, enu_output);
  }
}
tf2::Quaternion ImuNedToEnu(double x, double y, double z, double w) {
  double roll, pitch, yaw;

  tf2::Quaternion quat(x, y, z, w);
  tf2::Matrix3x3(quat).getRPY(roll, pitch, yaw);
  quat.setRPY(roll, -pitch, -yaw);

  return quat;
}

}  //  namespace imu_vn_100
