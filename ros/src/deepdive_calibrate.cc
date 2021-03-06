// ROS includes
#include <ros/ros.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/static_transform_broadcaster.h>

// We will use OpenCV to bootstrap solution
#include <opencv2/calib3d/calib3d.hpp>

// Third-party includes
#include <geometry_msgs/TransformStamped.h>
#include <visualization_msgs/MarkerArray.h>
#include <nav_msgs/Path.h>
#include <std_srvs/Trigger.h>

// Non-standard datra messages
#include <deepdive_ros/Light.h>
#include <deepdive_ros/Lighthouses.h>
#include <deepdive_ros/Trackers.h>

// Ceres and logging
#include <Eigen/Core>
#include <Eigen/Geometry>

// C++ libraries
#include <map>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

// Shared local code
#include "deepdive.hh"

// GLOBAL PARAMETERS

// List of lighthouses
TrackerMap trackers_;
LighthouseMap lighthouses_;
MeasurementMap measurements_;
CorrectionMap corrections_;

// Global strings
std::string calfile_ = "deepdive.tf2";
std::string frame_world_ = "world";     // World frame
std::string frame_vive_ = "vive";       // Vive frame
std::string frame_body_ = "body";       // EKF / external solution
std::string frame_truth_ = "truth";     // Vive solution

// Rejection thresholds
int thresh_count_ = 4;
double thresh_angle_ = 60.0;
double thresh_duration_ = 1.0;

// Are we running in "offline" mode
bool offline_ = false;

// Should we publish rviz markers
bool visualize_ = true;

// Are we recording right now?
bool recording_ = false;

// Should we correct lighthouse measurements
bool correct_ = false;

// Graph resolution
double res_ = 0.1;

// Tracker  centroid from body frame
std::vector<double> offset_;

// World -> vive registatration
double wTv_[6];

// Sensor visualization publisher
ros::Publisher pub_truth_;
std::map<std::string, ros::Publisher> pub_sensors_;
std::map<std::string, std::map<std::string, ros::Publisher>> pub_path_;

// Timer for managing offline
ros::Timer timer_;

// Jointly solve
bool Solve() {
  // Check that we have enough measurements
  if (measurements_.empty()) {
    ROS_WARN("Insufficient measurements received, so cannot solve problem.");
    return false;
  } else {
    double t = (measurements_.rbegin()->first
      - measurements_.begin()->first).toSec();
    ROS_INFO_STREAM("Processing " << measurements_.size()
      << " measurements running for " << t << " seconds from "
      << measurements_.begin()->first << " to "
      << measurements_.rbegin()->first);
  }

  // Check corrections
  if (corrections_.empty()) {
    ROS_INFO("No corrections in dataset. Assuming first body pose at origin.");
  } else {
    double t = (corrections_.rbegin()->first - corrections_.begin()->first).toSec();
    ROS_INFO_STREAM("Processing " << corrections_.size()
      << " corrections running for " << t << " seconds from "
      << corrections_.begin()->first << " to "
      << corrections_.rbegin()->first);
  }

  // Data storage for the upcoming steps

  typedef std::map<ros::Time,             // Time
            std::map<uint8_t,             // Sensor
              std::map<uint8_t,           // Axis
                std::vector<double>       // Sample
              >
            >
          > Bundle;
  std::map<std::string,                   // Tracker
    std::map<std::string,                 // Lighthouse
      Bundle
    >
  > bundle;

  std::map<ros::Time, double[6]> cor;     // Corrections

  double height = 0.0;                    // Average height

  // We bundle measurements into into bins of width "resolution". This allows
  // us to take the average of the measurements to improve accuracy
  {
    ROS_INFO("Bundling measurements into larger discrete time units.");
    MeasurementMap::iterator mt;
    for (mt = measurements_.begin(); mt != measurements_.end(); mt++) {
      std::string const& tserial = mt->second.light.header.frame_id;
      std::string const& lserial = mt->second.light.lighthouse;
      size_t const& a = mt->second.light.axis;
      ros::Time t = ros::Time(round(mt->first.toSec() / res_) * res_);
      std::vector<deepdive_ros::Pulse>::iterator pt;
      for (pt = mt->second.light.pulses.begin(); pt != mt->second.light.pulses.end(); pt++)
        bundle[tserial][lserial][t][pt->sensor][a].push_back(pt->angle);
    }
    ROS_INFO("Bundling corrections into larger discrete time units.");
    CorrectionMap::iterator ct;
    for (ct = corrections_.begin(); ct != corrections_.end(); ct++) {
      ros::Time t = ros::Time(round(ct->first.toSec() / res_) * res_);
      Eigen::Quaterniond q(
        ct->second.transform.rotation.w,
        ct->second.transform.rotation.x,
        ct->second.transform.rotation.y,
        ct->second.transform.rotation.z);
      Eigen::AngleAxisd aa(q.toRotationMatrix());
      cor[t][0] = ct->second.transform.translation.x;
      cor[t][1] = ct->second.transform.translation.y;
      cor[t][2] = ct->second.transform.translation.z;
      cor[t][3] = aa.angle() * aa.axis()[0];
      cor[t][4] = aa.angle() * aa.axis()[1];
      cor[t][5] = aa.angle() * aa.axis()[2];
      height += ct->second.transform.translation.z;
    }
    if (!corrections_.empty())
      height /= corrections_.size();
    ROS_INFO_STREAM("Average height is " << height << " meters");
  }

  // Data storage for next step

  typedef std::map<std::string,               // Tracker
    std::map<ros::Time,               // Time
      std::map<std::string,           // Lighthouse
        double[6]                     // Transform
      >
    >
  > PoseMap;

  PoseMap poses;

  // We are going to estimate the pose of each slave lighthouse in the frame
  // of the master lighthouse (vive frame) using PNP. We can think of the
  // two lighthouses as a stereo pair that are looking at a set of corresp-
  // ondences (photosensors). We want to calibrate this stereo pair.
  {
    ROS_INFO("Using P3P to estimate pose sequence in every lighthouse frame.");
    double fov = 2.0944;                          // 120deg FOV
    double w = 1.0;                               // 1m synthetic image plane
    double z = w / (2.0 * std::tan(fov / 2.0));   // Principle distance
    uint32_t count = 0;                           // Track num transforms
    // Iterate over lighthouses
    LighthouseMap::iterator lt;
    for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++) {
      // Iterate over trackers
      TrackerMap::iterator tt;
      for (tt = trackers_.begin(); tt != trackers_.end(); tt++) {
        ROS_INFO_STREAM("- Slave " << lt->first << " and tracker " << tt->first);
        // Iterate over time epochs
        Bundle::iterator bt;
        for (bt = bundle[tt->first][lt->first].begin();
          bt != bundle[tt->first][lt->first].end(); bt++) {
          // One for each time instance
          std::vector<cv::Point3f> obj;
          std::vector<cv::Point2f> img;
          // Try and find correspondences for every possible sensor
          for (uint8_t s = 0; s < NUM_SENSORS; s++) {
            // Mean angles for the <lighthouse, axis>
            double angles[2];
            // Check that we have azimuth/elevation for both lighthouses
            if (!Mean(bundle[tt->first][lt->first][bt->first][s][0], angles[0]) ||
                !Mean(bundle[tt->first][lt->first][bt->first][s][1], angles[1]))
              continue;
            // Correct the angles using the lighthouse parameters
            Correct(lt->second.params, angles, correct_);
            // Push on the correct world sensor position
            obj.push_back(cv::Point3f(
              trackers_[tt->first].sensors[s * 6 + 0],
              trackers_[tt->first].sensors[s * 6 + 1],
              trackers_[tt->first].sensors[s * 6 + 2]));
            // Push on the coordinate in the slave image plane
            img.push_back(cv::Point2f(z * tan(angles[0]), z * tan(angles[1])));
          }
          // In the case that we have 4 or more measurements, then we can try
          // and estimate the trackers location in the lighthouse frame.
          if (obj.size() > 6) {
            cv::Mat cam = cv::Mat::eye(3, 3, cv::DataType<double>::type);
            cv::Mat dist;
            cam.at<double>(0, 0) = z;
            cam.at<double>(1, 1) = z;
            cv::Mat R(3, 1, cv::DataType<double>::type);
            cv::Mat T(3, 1, cv::DataType<double>::type);
            cv::Mat C(3, 3, cv::DataType<double>::type);
            if (cv::solvePnPRansac(obj, img, cam, dist, R, T,  false,
              100, 8.0, 0.99, cv::noArray(), cv::SOLVEPNP_UPNP)) {
              cv::Rodrigues(R, C);
              Eigen::Matrix3d rot;
              for (size_t r = 0; r < 3; r++)
                for (size_t c = 0; c < 3; c++)
                  rot(r, c) = C.at<double>(r, c);
              Eigen::AngleAxisd aa(rot);
              poses[tt->first][bt->first][lt->first][0] = T.at<double>(0, 0);
              poses[tt->first][bt->first][lt->first][1] = T.at<double>(1, 0);
              poses[tt->first][bt->first][lt->first][2] = T.at<double>(2, 0);
              poses[tt->first][bt->first][lt->first][3] = aa.angle() * aa.axis()[0];
              poses[tt->first][bt->first][lt->first][4] = aa.angle() * aa.axis()[1];
              poses[tt->first][bt->first][lt->first][5] = aa.angle() * aa.axis()[2];
              count++;
            }
          }
        }
      }
    }
    ROS_INFO_STREAM("Using " << count << " PNP solutions");
  }
  // We now have a separate pose sequence for each tracker in each lighthouse
  // frame. We now need to find the transform from the slave to master light-
  // house in a way that projects one pose sequence into the other.
  {
    ROS_INFO("Estimating master -> slave lighthouse transforms.");
    // Add residual blocks to the problem
    LighthouseMap::iterator lm = lighthouses_.begin();  // First is master
    LighthouseMap::iterator lt;                         //
    for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++) {
      // Master lighthouse
      if (lt == lighthouses_.begin()) {
        for (size_t i = 0; i < 6; i++)
          lt->second.vTl[0] = 0;
        continue;
      }
      // Correspondences
      std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> corresp;
      // Slave lighthouse
      if (lt != lighthouses_.begin()) {
        TrackerMap::iterator tt;
        for (tt = trackers_.begin(); tt != trackers_.end(); tt++) {
          std::map<ros::Time, std::map<std::string, double[6]>>::iterator pt;
          for (pt = poses[tt->first].begin(); pt != poses[tt->first].end(); pt++) {
            // We must have a pose for both the master AND the slave
            if (pt->second.find(lt->first) == pt->second.end() ||
                pt->second.find(lm->first) == pt->second.end()) continue;
            // If we get here, then we have a correspondence
            corresp.push_back(std::pair<Eigen::Vector3d, Eigen::Vector3d>(
              Eigen::Vector3d(
                pt->second[lt->first][0],
                pt->second[lt->first][1],
                pt->second[lt->first][2]
              ),
              Eigen::Vector3d(
                pt->second[lm->first][0],
                pt->second[lm->first][1],
                pt->second[lm->first][2])
              )
            );
          }
        }
      }
      // Run kabsch to determine the projection from the slave to master
      Eigen::Matrix<double, 3, Eigen::Dynamic> pti(3, corresp.size());
      Eigen::Matrix<double, 3, Eigen::Dynamic> ptj(3, corresp.size());
      for (size_t i = 0; i < corresp.size(); i++) {
        pti.block<3, 1>(0, i) = corresp[i].first;
        ptj.block<3, 1>(0, i) = corresp[i].second;
      }
      // Perform a KABSCH transform on the two matrices
      ROS_INFO_STREAM("- Using " << corresp.size() << " correspondences");
      Eigen::Affine3d A;
      if (Kabsch<double>(pti, ptj, A, false))
        ROS_INFO_STREAM("- Solution " << A.translation().norm());
      else
        ROS_INFO("- Solution not found");
      // Write the solution
      lt->second.vTl[0] = A.translation()[0];
      lt->second.vTl[1] = A.translation()[1];
      lt->second.vTl[2] = A.translation()[2];
      Eigen::AngleAxisd aa(A.linear());
      lt->second.vTl[3] =  aa.angle() * aa.axis()[0];
      lt->second.vTl[4] =  aa.angle() * aa.axis()[1];
      lt->second.vTl[5] =  aa.angle() * aa.axis()[2];
    }
  }

  // REGISTRATION - for the granite lab we know that the trackers are mounted
  // symmetrically about the body frame. Consequently, the average X-Y pose
  // of the trackers is a good approximation of the body X-Y pose. The height
  // is fixed, so we can take the average from the corrections 
  {
    ROS_INFO("Using corrections to register vive to world frame.");
    // The vive frame is the same as the maste rlighthouse
    std::string lm = lighthouses_.begin()->first;
    // This will store the correspondences
    std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> corresp;
    // Iterate over all corrections
    std::map<ros::Time, double[6]>::iterator ct;
    for (ct = cor.begin(); ct != cor.end(); ct++) {
      double x = 0.0, y = 0.0, z = 0.0;
      size_t n = 0;
      TrackerMap::iterator tt;
      for (tt = trackers_.begin(); tt != trackers_.end(); tt++) {
        if (poses.find(tt->first) != poses.end() &&
          poses[tt->first].find(ct->first) != poses[tt->first].end() &&
          poses[tt->first][ct->first].find(lm) != poses[tt->first][ct->first].end()) {
          x += poses[tt->first][ct->first][lm][0];
          y += poses[tt->first][ct->first][lm][1];
          z += poses[tt->first][ct->first][lm][2];
          n++;
        }
      }
      // Only if we have data from all trackers
      if (n == trackers_.size()) {
        corresp.push_back(
          std::pair<Eigen::Vector3d, Eigen::Vector3d>(
            Eigen::Vector3d(x / n, y / n, z / n),
            Eigen::Vector3d(
              ct->second[0] + offset_[0],
              ct->second[1] + offset_[1],
              ct->second[2] + offset_[2]
            )
          )
        );
      }
    }
    // Run kabsch to determine the projection from the slave to master
    Eigen::Matrix<double, 3, Eigen::Dynamic> pti(3, corresp.size());
    Eigen::Matrix<double, 3, Eigen::Dynamic> ptj(3, corresp.size());
    for (size_t i = 0; i < corresp.size(); i++) {
      pti.block<3, 1>(0, i) = corresp[i].first;
      ptj.block<3, 1>(0, i) = corresp[i].second;
    }
    // Perform a KABSCH transform on the two matrices
    ROS_INFO_STREAM("- Using " << corresp.size() << " correspondences");
    Eigen::Affine3d A;
    if (Kabsch<double>(pti, ptj, A, false))
      ROS_INFO_STREAM("- Solution " << A.translation().norm());
    else
      ROS_INFO("- No correspondences so vive -> world frame is identity");
    // Write the solution
    wTv_[0] = A.translation()[0];
    wTv_[1] = A.translation()[1];
    wTv_[2] = A.translation()[2];
    Eigen::AngleAxisd aa(A.linear());
    wTv_[3] = aa.angle() * aa.axis()[0];
    wTv_[4] = aa.angle() * aa.axis()[1];
    wTv_[5] = aa.angle() * aa.axis()[2];
  }

  // We now have a great estimate of the slave -> master lighthous transforms,
  // sensor trajectories, and global registration
  {
    SendTransforms(frame_world_, frame_vive_, frame_body_,
      wTv_, lighthouses_, trackers_);
    // Write the solution to a config file
    if (WriteConfig(calfile_, frame_world_, frame_vive_, frame_body_,
      wTv_, lighthouses_, trackers_))
      ROS_INFO_STREAM("Calibration written to " << calfile_);
    else
      ROS_INFO_STREAM("Could not write calibration to" << calfile_);
    // Print the trajectory of the body-frame in the world-frame
    if (visualize_) {
      // Estimates
      LighthouseMap::iterator lt;
      for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++) {
        TrackerMap::iterator tt;
        for (tt = trackers_.begin(); tt != trackers_.end(); tt++) {
          // Create a path
          nav_msgs::Path msg;
          msg.header.stamp = ros::Time::now();
          msg.header.frame_id = lt->first;
          // Iterate over timestamp
          std::map<ros::Time, std::map<std::string, double[6]>>::iterator pt;
          for (pt = poses[tt->first].begin(); pt != poses[tt->first].end(); pt++) {
            // Don't plot if the data doesn't exist
            if (pt->second.find(lt->first) == pt->second.end())
              continue;
            // Convert axis angle to quaternion
            Eigen::Vector3d v(
              pt->second[lt->first][3],
              pt->second[lt->first][4],
              pt->second[lt->first][5]);
            Eigen::AngleAxisd aa = Eigen::AngleAxisd::Identity();
            if (v.norm() > 0) {
              aa.angle() = v.norm();
              aa.axis() = v.normalized();
            }
            Eigen::Quaterniond q(aa);
            // PUblish pose
            geometry_msgs::PoseStamped ps;
            ps.header.stamp = pt->first;
            ps.header.frame_id = lt->first;
            ps.pose.position.x = pt->second[lt->first][0];
            ps.pose.position.y = pt->second[lt->first][1];
            ps.pose.position.z = pt->second[lt->first][2];
            ps.pose.orientation.w = q.w();
            ps.pose.orientation.x = q.x();
            ps.pose.orientation.y = q.y();
            ps.pose.orientation.z = q.z();
            msg.poses.push_back(ps);
          }
          pub_path_[lt->first][tt->first].publish(msg);
        }
      }
      // Create a path
      nav_msgs::Path msg;
      msg.header.stamp = ros::Time::now();
      msg.header.frame_id = frame_world_;
      std::map<ros::Time, double[6]>::iterator ct;
      for (ct = cor.begin(); ct != cor.end(); ct++) {
        Eigen::Vector3d v(ct->second[3], ct->second[4], ct->second[5]);
        Eigen::AngleAxisd aa = Eigen::AngleAxisd::Identity();
        if (v.norm() > 0) {
          aa.angle() = v.norm();
          aa.axis() = v.normalized();
        }
        Eigen::Quaterniond q(aa);
        geometry_msgs::PoseStamped ps;
        ps.header.stamp = ct->first;
        ps.header.frame_id = frame_world_;
        ps.pose.position.x = ct->second[0];
        ps.pose.position.y = ct->second[1];
        ps.pose.position.z = ct->second[2];
        ps.pose.orientation.w = q.w();
        ps.pose.orientation.x = q.x();
        ps.pose.orientation.y = q.y();
        ps.pose.orientation.z = q.z();
        msg.poses.push_back(ps);
      }
      pub_truth_.publish(msg);
    }
  }
  return true;
}

// MESSAGE CALLBACKS

void LightCallback(deepdive_ros::Light::ConstPtr const& msg) {
  // Reset the timer use din offline mode to determine the end of experiment
  timer_.stop();
  timer_.start();
  // Check that we are recording and that the tracker/lighthouse is ready
  if (!recording_ ||
    trackers_.find(msg->header.frame_id) == trackers_.end() ||
    lighthouses_.find(msg->lighthouse) == lighthouses_.end() ||
    !trackers_[msg->header.frame_id].ready ||
    !lighthouses_[msg->lighthouse].ready) return;
  // Copy over the data
  size_t deleted = 0;
  deepdive_ros::Light data = *msg;
  std::vector<deepdive_ros::Pulse>::iterator it = data.pulses.end();
  while (it-- > data.pulses.begin()) {
    // std::cout << it->duration << std::endl;
    if (it->angle > thresh_angle_ / 57.2958 ||     // Check angle
        it->duration < thresh_duration_ / 1e6) {  // Check duration
      it = data.pulses.erase(it);
      deleted++;
    }
  }
  if (data.pulses.size() < thresh_count_)
    return; 
  // Add the data
  measurements_[ros::Time::now()].light = data;
}

bool TriggerCallback(std_srvs::Trigger::Request  &req,
                     std_srvs::Trigger::Response &res)
{
  if (!recording_) {
    res.success = true;
    res.message = "Recording started.";
  }
  if (recording_) {
    // Solve the problem
    res.success = Solve();
    if (res.success)
      res.message = "Recording stopped. Solution found.";
    else
      res.message = "Recording stopped. Solution not found.";
    // Clear all the data and corrections
    measurements_.clear();
  }
  // Toggle recording state
  recording_ = !recording_;
  // Success
  return true;
}

// When corrections arrive
void CorrectionCallback(tf2_msgs::TFMessage::ConstPtr const& msg) {
  // Check that we are recording and that the tracker/lighthouse is ready
  if (!recording_)
    return;
  std::vector<geometry_msgs::TransformStamped>::const_iterator it;
  for (it = msg->transforms.begin(); it != msg->transforms.end(); it++) {
    if (it->header.frame_id == frame_world_ &&
        it->child_frame_id == frame_body_) {
      corrections_[ros::Time::now()] = *it;
    }
  }
}

// Fake a trigger when the timer expires
void TimerCallback(ros::TimerEvent const& event) {
  std_srvs::Trigger::Request req;
  std_srvs::Trigger::Response res;
  TriggerCallback(req, res);
}

// Called when a new lighthouse appears
void NewLighthouseCallback(LighthouseMap::iterator lighthouse) {
  ROS_INFO_STREAM("Found lighthouse " << lighthouse->first);
}

// Called when a new tracker appears
void NewTrackerCallback(TrackerMap::iterator tracker) {
  ROS_INFO_STREAM("Found tracker " << tracker->first);
  if (visualize_) {
    visualization_msgs::MarkerArray msg;
    std::map<std::string, Tracker>::iterator jt;
    for (jt = trackers_.begin(); jt != trackers_.end(); jt++)  {
      for (uint16_t i = 0; i < NUM_SENSORS; i++) {
        // All this code just to convert a normal to a quaternion
        Eigen::Vector3d vfwd(jt->second.sensors[6*i+3],
          jt->second.sensors[6*i+4], jt->second.sensors[6*i+5]);
        if (vfwd.norm() > 0) {
          Eigen::Vector3d vdown(0.0, 0.0, 1.0);
          Eigen::Vector3d vright = vdown.cross(vfwd);
          vfwd = vfwd.normalized();
          vright = vright.normalized();
          vdown = vdown.normalized();
          Eigen::Matrix3d dcm;
          dcm << vfwd.x(), vright.x(), vdown.x(),
                 vfwd.y(), vright.y(), vdown.y(),
                 vfwd.z(), vright.z(), vdown.z();
          Eigen::Quaterniond q(dcm);
          // Now plot an arrow representing the normal
          visualization_msgs::Marker marker;
          marker.header.frame_id = jt->first + "/light";
          marker.header.stamp = ros::Time::now();
          marker.ns = jt->first;
          marker.id = i;
          marker.type = visualization_msgs::Marker::ARROW;
          marker.action = visualization_msgs::Marker::ADD;
          marker.pose.position.x = jt->second.sensors[6*i+0];
          marker.pose.position.y = jt->second.sensors[6*i+1];
          marker.pose.position.z = jt->second.sensors[6*i+2];
          marker.pose.orientation.w = q.w();
          marker.pose.orientation.x = q.x();
          marker.pose.orientation.y = q.y();
          marker.pose.orientation.z = q.z();
          marker.scale.x = 0.010;
          marker.scale.y = 0.001;
          marker.scale.z = 0.001;
          marker.color.a = 1.0;
          marker.color.r = 1.0;
          marker.color.g = 0.0;
          marker.color.b = 0.0;
          msg.markers.push_back(marker);
        }
      }
      pub_sensors_[tracker->first].publish(msg);
    }
  }
}

// MAIN ENTRY POINT

int main(int argc, char **argv) {
  // Initialize ROS and create node handle
  ros::init(argc, argv, "deepdive_calibrate");
  ros::NodeHandle nh("~");

  // If we are in offline mode when we will replay the data back at 10x the
  // speed, using it all to find a calibration solution for both the body
  // as a function of  time and the lighthouse positions.
  if (!nh.getParam("offline", offline_))
    ROS_FATAL("Failed to get if we are running in offline mode.");
  if (offline_) {
    ROS_INFO("We are in offline mode. Speed-up is possible.");
    recording_ = true;
  }

  // Reset the registration information
  for (size_t i = 0; i < 6; i++)
    wTv_[i] = 0;

  // Get the parent information
  if (!nh.getParam("calfile", calfile_))
    ROS_FATAL("Failed to get the calfile file.");

  // Get some global information
  if (!nh.getParam("frames/world", frame_world_))
    ROS_FATAL("Failed to get frames/world parameter.");
  if (!nh.getParam("frames/vive", frame_vive_))
    ROS_FATAL("Failed to get frames/vive parameter.");
  if (!nh.getParam("frames/body", frame_body_))
    ROS_FATAL("Failed to get frames/body parameter.");
  if (!nh.getParam("frames/truth", frame_truth_))
    ROS_FATAL("Failed to get frames/truth parameter.");

  // Get the thresholds
  if (!nh.getParam("thresholds/count", thresh_count_))
    ROS_FATAL("Failed to get threshods/count parameter.");
  if (!nh.getParam("thresholds/angle", thresh_angle_))
    ROS_FATAL("Failed to get thresholds/angle parameter.");
  if (!nh.getParam("thresholds/duration", thresh_duration_))
    ROS_FATAL("Failed to get thresholds/duration parameter.");

  // Tracking resolution
  if (!nh.getParam("resolution", res_))
    ROS_FATAL("Failed to get resolution parameter.");

  // Whether to apply light corrections
  if (!nh.getParam("correct", correct_))
    ROS_FATAL("Failed to get correct parameter.");

  // Visualization option
  if (!nh.getParam("visualize", visualize_))
    ROS_FATAL("Failed to get the visualize parameter.");

  // Get the offset of the tracker centroid and the body
  if (!nh.getParam("offset", offset_))
    ROS_FATAL("Failed to get the lighthouse transform.");
  if (offset_.size() != 3)
    ROS_FATAL("Failed to parse lighthouse transform.");

  // Get the parent information
  std::vector<std::string> lighthouses;
  if (!nh.getParam("lighthouses", lighthouses))
    ROS_FATAL("Failed to get the lighthouse list.");
  std::vector<std::string>::iterator it;
  for (it = lighthouses.begin(); it != lighthouses.end(); it++) {
    std::string serial;
    if (!nh.getParam(*it + "/serial", serial))
      ROS_FATAL("Failed to get the lighthouse serial.");
    std::vector<double> transform;
    if (!nh.getParam(*it + "/transform", transform))
      ROS_FATAL("Failed to get the lighthouse transform.");
    if (transform.size() != 7) {
      ROS_FATAL("Failed to parse lighthouse transform.");
      continue;
    }
    Eigen::Quaterniond q(transform[6], transform[3], transform[4], transform[5]);
    Eigen::AngleAxisd aa(q);
    lighthouses_[serial].vTl[0] = transform[0];
    lighthouses_[serial].vTl[1] = transform[1];
    lighthouses_[serial].vTl[2] = transform[2];
    lighthouses_[serial].vTl[3] = aa.angle() * aa.axis()[0];
    lighthouses_[serial].vTl[4] = aa.angle() * aa.axis()[1];
    lighthouses_[serial].vTl[5] = aa.angle() * aa.axis()[2];
    lighthouses_[serial].ready = false;
  }

  // Get the parent information
  std::vector<std::string> trackers;
  if (!nh.getParam("trackers", trackers))
    ROS_FATAL("Failed to get the tracker list.");
  std::vector<std::string>::iterator jt;
  for (jt = trackers.begin(); jt != trackers.end(); jt++) {
    std::string serial;
    if (!nh.getParam(*jt + "/serial", serial))
      ROS_FATAL("Failed to get the tracker serial.");
    std::vector<double> extrinsics;
    if (!nh.getParam(*jt + "/extrinsics", extrinsics))
      ROS_FATAL("Failed to get the tracker extrinsics.");
    if (extrinsics.size() != 7) {
      ROS_FATAL("Failed to parse tracker extrinsics.");
      continue;
    }
    Eigen::Quaterniond q( extrinsics[6], extrinsics[3], extrinsics[4], extrinsics[5]);
    Eigen::AngleAxisd aa(q);
    trackers_[serial].bTh[0] = extrinsics[0];
    trackers_[serial].bTh[1] = extrinsics[1];
    trackers_[serial].bTh[2] = extrinsics[2];
    trackers_[serial].bTh[3] = aa.angle() * aa.axis()[0];
    trackers_[serial].bTh[4] = aa.angle() * aa.axis()[1];
    trackers_[serial].bTh[5] = aa.angle() * aa.axis()[2];
    trackers_[serial].ready = false;
    // Publish sensor location and body trajectory 
    pub_truth_ = nh.advertise<nav_msgs::Path>("/truth", 10, true);
    pub_sensors_[serial] = nh.advertise<visualization_msgs::MarkerArray>(
      "/sensors/" + *jt, 10, true);
    LighthouseMap::iterator lt;
    for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++)
      pub_path_[lt->first][serial] = nh.advertise<nav_msgs::Path>(
        "/path/" + *jt + "/" + lt->first, 10, true);
  }

  // Send the transforms out
  SendTransforms(frame_world_, frame_vive_, frame_body_,
    wTv_, lighthouses_, trackers_);

  // Subscribe to tracker and lighthouse updates
  ros::Subscriber sub_tracker  = 
    nh.subscribe<deepdive_ros::Trackers>("/trackers", 1000, std::bind(
      TrackerCallback, std::placeholders::_1, std::ref(trackers_),
        NewTrackerCallback));
  ros::Subscriber sub_lighthouse = 
    nh.subscribe<deepdive_ros::Lighthouses>("/lighthouses", 1000, std::bind(
      LighthouseCallback, std::placeholders::_1, std::ref(lighthouses_),
        NewLighthouseCallback));
  ros::Subscriber sub_light =
    nh.subscribe("/light", 1000, LightCallback);
  ros::Subscriber sub_corrections =
    nh.subscribe("/tf", 1000, CorrectionCallback);
  ros::ServiceServer service =
    nh.advertiseService("/trigger", TriggerCallback);

  // Setup a timer to automatically trigger solution on end of experiment
  timer_ = nh.createTimer(ros::Duration(1.0), TimerCallback, true, false);

  // Block until safe shutdown
  ros::spin();

  // Success!
  return 0;
}