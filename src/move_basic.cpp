/*
 * Copyright (c) 2017-9, Ubiquity Robotics
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 *
 */

#include <ros/ros.h>
#include <tf/transform_datatypes.h>
#include <tf2/LinearMath/Transform.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Twist.h>
#include <geometry_msgs/Vector3.h>
#include <nav_msgs/Path.h>
#include <std_msgs/Float32.h>
#include <move_base_msgs/MoveBaseAction.h>
#include <string>
#include <actionlib/server/simple_action_server.h>
#include <dynamic_reconfigure/server.h>
#include "move_basic/collision_checker.h"
#include "move_basic/queued_action_server.h"
#include <move_basic/MovebasicConfig.h>

typedef actionlib::QueuedActionServer<move_base_msgs::MoveBaseAction> MoveBaseActionServer;

class MoveBasic {
  private:
    ros::Subscriber goalSub;

    ros::Publisher goalPub;
    ros::Publisher cmdPub;
    ros::Publisher pathPub;
    ros::Publisher obstacle_dist_pub;
    ros::Publisher errorPub;

    std::unique_ptr<MoveBaseActionServer> actionServer;
    std::unique_ptr<CollisionChecker> collision_checker;
    std::unique_ptr<ObstaclePoints> obstacle_points;

    tf2_ros::Buffer tfBuffer;
    tf2_ros::TransformListener listener;

    double minTurningVelocity;
    double maxTurningVelocity;
    double angularAcceleration;
    double angularTolerance;
    double maxLateralVelocity;

    double maxLinearVelocity;
    double linearAcceleration;
    double linearTolerance;

    // PID parameters for controlling lateral error
    double lateralKp;
    double lateralKi;
    double lateralKd;
    double linGain;
    double rotGain;
    double velThreshold;

    double abortTimeoutSecs;
    double obstacleWaitThreshold;
    double forwardObstacleThreshold;
    double localizationLatency;

    std::string preferredPlanningFrame;
    std::string alternatePlanningFrame;
    std::string preferredDrivingFrame;
    std::string alternateDrivingFrame;
    std::string baseFrame;

    double minSideDist;
    double sideRecoverWeight;

    float forwardObstacleDist;
    float leftObstacleDist;
    float rightObstacleDist;
    tf2::Vector3 forwardLeft;
    tf2::Vector3 forwardRight;
    double reverseWithoutTurningThreshold;

    dynamic_reconfigure::Server<move_basic::MovebasicConfig> dr_srv;

    void dynamicReconfigCallback(move_basic::MovebasicConfig& config, uint32_t level);
    void goalCallback(const geometry_msgs::PoseStamped::ConstPtr &msg);
    void executeAction(const move_base_msgs::MoveBaseGoalConstPtr& goal);
    void drawLine(double x0, double y0, double x1, double y1);
    void sendCmd(double angular, double linear);
    void abortGoal(const std::string msg);

    bool getTransform(const std::string& from, const std::string& to,
                      tf2::Transform& tf);
    bool transformPose(const std::string& from, const std::string& to,
                       const tf2::Transform& in, tf2::Transform& out);

  public:
    MoveBasic();

    void run();

    bool moveLinear(tf2::Transform& goalInDriving,
                    const std::string& drivingFrame);
    bool rotate(double requestedYaw,
                const std::string& drivingFrame);

    tf2::Transform goalInPlanning;
};


// Radians to degrees

static double rad2deg(double rad)
{
    return rad * 180.0 / M_PI;
}

// Adjust angle to be between -PI and PI

static void normalizeAngle(double& angle)
{
    if (angle < -M_PI) {
         angle += 2 * M_PI;
    }
    if (angle > M_PI) {
        angle -= 2 * M_PI;
    }
}


// retreive the 3 DOF we are interested in from a Transform

static void getPose(const tf2::Transform& tf, double& x, double& y, double& yaw)
{
    tf2::Vector3 trans = tf.getOrigin();
    x = trans.x();
    y = trans.y();

    double roll, pitch;
    tf.getBasis().getRPY(roll, pitch, yaw);
}


// Constructor

MoveBasic::MoveBasic(): tfBuffer(ros::Duration(3.0)),
                        listener(tfBuffer)
{
    ros::NodeHandle nh("~");

    // Velocity parameters
    nh.param<double>("min_turning_velocity", minTurningVelocity, 0.02);
    nh.param<double>("max_turning_velocity", maxTurningVelocity, 1.0);
    nh.param<double>("angular_acceleration", angularAcceleration, 0.3);
    nh.param<double>("max_linear_velocity", maxLinearVelocity, 0.5);
    nh.param<double>("linear_acceleration", linearAcceleration, 0.1);
    nh.param<double>("angular_tolerance", angularTolerance, 0.01);
    nh.param<double>("linear_tolerance", linearTolerance, 0.1);

    // Parameters for turn PID
    nh.param<double>("lateral_kp", lateralKp, 2.0);
    nh.param<double>("lateral_ki", lateralKi, 0.0);
    nh.param<double>("lateral_kd", lateralKd, 20.0);

    // Gain for velocities
    nh.param<double>("linear_gain", linGain, 1.0);
    nh.param<double>("rotational_gain", rotGain, 2.5);

    // Navigation test
    nh.param<double>("velocity_threshold", velThreshold, 0.1);

    // Minimum distance to maintain at each side
    nh.param<double>("min_side_dist", minSideDist, 0.3);

    // Maximum angular velocity during linear portion
    nh.param<double>("max_lateral_velocity", maxLateralVelocity, 0.5);

    // Weighting of turning to recover from avoiding side obstacles
    nh.param<double>("side_recover_weight", sideRecoverWeight, 1.0);

    // how long to wait after moving to be sure localization is accurate
    nh.param<double>("localization_latency", localizationLatency, 0.5);

    // Time which the robot can be driving away from the goal
    nh.param<double>("abort_timeout", abortTimeoutSecs, 5.0);

    // how long to wait for an obstacle to disappear
    nh.param<double>("obstacle_wait_threshold", obstacleWaitThreshold, 60.0);

    // if distance <  velocity times this we stop
    nh.param<double>("forward_obstacle_threshold", forwardObstacleThreshold, 0.5);

    // Reverse distances for which rotation won't be performed
    nh.param<double>("reverse_without_turning_threshold",
                      reverseWithoutTurningThreshold, 0.5);

    nh.param<std::string>("preferred_planning_frame",
                          preferredPlanningFrame, "");
    nh.param<std::string>("alternate_planning_frame",
                          alternatePlanningFrame, "odom");
    nh.param<std::string>("preferred_driving_frame",
                          preferredDrivingFrame, "map");
    nh.param<std::string>("alternate_driving_frame",
                          alternateDrivingFrame, "odom");
    nh.param<std::string>("base_frame", baseFrame, "base_footprint");

    dynamic_reconfigure::Server<move_basic::MovebasicConfig>::CallbackType f;
    f = boost::bind(&MoveBasic::dynamicReconfigCallback, this, _1, _2);
    dr_srv.setCallback(f);

    cmdPub = ros::Publisher(nh.advertise<geometry_msgs::Twist>("/cmd_vel", 1));
    pathPub = ros::Publisher(nh.advertise<nav_msgs::Path>("/plan", 1));

    obstacle_dist_pub =
        ros::Publisher(nh.advertise<geometry_msgs::Vector3>("/obstacle_distance", 1));
    errorPub =
        ros::Publisher(nh.advertise<geometry_msgs::Vector3>("/lateral_error", 1));

    goalSub = nh.subscribe("/move_base_simple/goal", 1,
                            &MoveBasic::goalCallback, this);

    ros::NodeHandle actionNh("");

    actionServer.reset(new MoveBaseActionServer(actionNh, "move_base",
	boost::bind(&MoveBasic::executeAction, this, _1)));

    actionServer->start();
    goalPub = actionNh.advertise<move_base_msgs::MoveBaseActionGoal>(
      "/move_base/goal", 1);

    obstacle_points.reset(new ObstaclePoints(nh, tfBuffer));
    collision_checker.reset(new CollisionChecker(nh, tfBuffer, *obstacle_points));

    ROS_INFO("Move Basic ready");
}


// Lookup the specified transform, returns true on success

bool MoveBasic::getTransform(const std::string& from, const std::string& to,
                             tf2::Transform& tf)
{
    try {
        geometry_msgs::TransformStamped tfs =
            tfBuffer.lookupTransform(to, from, ros::Time(0));
        tf2::fromMsg(tfs.transform, tf);
        return true;
    }
    catch (tf2::TransformException &ex) {
         return false;
    }
}

// Transform a pose from one frame to another

bool MoveBasic::transformPose(const std::string& from, const std::string& to,
                              const tf2::Transform& in, tf2::Transform& out)
{
    tf2::Transform tf;
    if (!getTransform(from, to, tf)) {
        return false;
    }
    out = tf * in;
    return true;
}

// Dynamic reconfigure

void MoveBasic::dynamicReconfigCallback(move_basic::MovebasicConfig& config, uint32_t level){
    minTurningVelocity = config.min_turning_velocity;
    maxTurningVelocity = config.max_turning_velocity;
    maxLateralVelocity = config.max_lateral_velocity;
    angularAcceleration = config.angular_acceleration;
    maxLinearVelocity = config.max_linear_velocity;
    linearAcceleration = config.linear_acceleration;
    angularTolerance = config.angular_tolerance;
    linearTolerance = config.linear_tolerance;
    localizationLatency = config.localization_latency;
    lateralKp = config.lateral_kp;
    lateralKi = config.lateral_ki;
    lateralKd = config.lateral_kd;
    linGain = config.linear_gain;
    rotGain = config.rotational_gain;
    velThreshold = config.velocity_threshold;
    minSideDist = config.min_side_dist;
    sideRecoverWeight = config.side_recover_weight;
    abortTimeoutSecs = config.abort_timeout;
    obstacleWaitThreshold = config.obstacle_wait_threshold;
    forwardObstacleThreshold = config.forward_obstacle_threshold;
    reverseWithoutTurningThreshold = config.reverse_without_turning_threshold;

    ROS_WARN("Parameter change detected");
}


// Called when a simple goal message is received

void MoveBasic::goalCallback(const geometry_msgs::PoseStamped::ConstPtr &msg)
{
    ROS_INFO("MoveBasic: Received simple goal");

    // send the goal to the action server
    move_base_msgs::MoveBaseActionGoal actionGoal;
    actionGoal.header.stamp = ros::Time::now();
    actionGoal.goal.target_pose = *msg;

    goalPub.publish(actionGoal);
}


// Abort goal and print message

void MoveBasic::abortGoal(const std::string msg)
{
    actionServer->setAborted(move_base_msgs::MoveBaseResult(), msg);
    ROS_ERROR("%s", msg.c_str());
}


// Called when an action goal is received

void MoveBasic::executeAction(const move_base_msgs::MoveBaseGoalConstPtr& msg)
{
    /*
      Plan a path that involves rotating to face the goal, going straight towards it,
      and then rotating for the final orientation.

      It is assumed that we are dealing with imperfect localization data:
      map->base_link is accurate but may be delayed and is at a slow rate
      odom->base_link is frequent, but drifts, particularly after rotating
      To counter these issues, we plan in the map frame, and wait localizationLatency
      after each step, and execute in the odom frame.
    */

    tf2::Transform goal;
    tf2::fromMsg(msg->target_pose.pose, goal);
    std::string frameId = msg->target_pose.header.frame_id;

    // Needed for RobotCommander
    if (frameId[0] == '/')
        frameId = frameId.substr(1);

    double x, y, yaw;
    getPose(goal, x, y, yaw);

    ROS_INFO("MoveBasic: Received goal %f %f %f %s", x, y, rad2deg(yaw), frameId.c_str());

    if (std::isnan(yaw)) {
        abortGoal("MoveBasic: Aborting goal because an invalid orientation was specified");
        return;
    }

    // The pose of the robot planning frame MUST be known initially, and may or may not
    // be known after that.
    // The pose of the robot in the driving frame MUST be known at all times.
    // An empty planning frame means to use what ever frame the goal is specified in.
    double goalYaw;
    std::string planningFrame;
    if (preferredPlanningFrame == "") {
       planningFrame = frameId;
       goalInPlanning = goal;
       ROS_INFO("Planning in goal frame: %s\n", planningFrame.c_str());
    }
    else if (!transformPose(frameId, preferredPlanningFrame, goal, goalInPlanning)) {
        ROS_WARN("MoveBasic: Will attempt to plan in %s frame", alternatePlanningFrame.c_str());
        if (!transformPose(frameId, alternatePlanningFrame, goal,
            goalInPlanning)) {
            abortGoal("MoveBasic: No localization available for planning");
            return;
        }
        planningFrame = alternatePlanningFrame;
        goalYaw = yaw;
    }
    else {
        planningFrame = preferredPlanningFrame;
    }

    getPose(goalInPlanning, x, y, goalYaw);
    ROS_INFO("MoveBasic: Goal in %s  %f %f %f", planningFrame.c_str(),
             x, y, rad2deg(goalYaw));

    // Publish our planned path
    nav_msgs::Path path;
    geometry_msgs::PoseStamped p0, p1;
    path.header.frame_id = frameId;
    p0.pose.position.x = x;
    p0.pose.position.y = y;
    p0.header.frame_id = frameId;
    path.poses.push_back(p0);

    tf2::Transform poseFrameId;
    if (!getTransform(baseFrame, frameId, poseFrameId)) {
         abortGoal("MoveBasic: Cannot determine robot pose in goal frame");
         return;
    }
    getPose(poseFrameId, x, y, yaw);
    p1.pose.position.x = x;
    p1.pose.position.y = y;
    p1.header.frame_id = frameId;
    path.poses.push_back(p1);

    pathPub.publish(path);

    std::string drivingFrame;
    tf2::Transform goalInDriving;
    tf2::Transform currentDrivingBase;
    // Should be at time of goal message
    if (!getTransform(preferredDrivingFrame, baseFrame, currentDrivingBase)) {
         ROS_WARN("MoveBasic: %s not available, attempting to drive using %s frame",
                  preferredDrivingFrame.c_str(), alternateDrivingFrame.c_str());
         if (!getTransform(alternateDrivingFrame,
                           baseFrame, currentDrivingBase)) {
             abortGoal("MoveBasic: Cannot determine robot pose in driving frame");
             return;
         }
         else {
             drivingFrame = alternateDrivingFrame;
         }
    }
    else {
      drivingFrame = preferredDrivingFrame;
    }

    if (!transformPose(frameId, drivingFrame, goal, goalInDriving)) {
         abortGoal("MoveBasic: Cannot determine goal pose in driving frame");
         return;
    }

    tf2::Transform goalInBase = currentDrivingBase * goalInDriving;
    {
       double x, y, yaw;
       getPose(goalInBase, x, y, yaw);
       ROS_INFO("MoveBasic: Goal in %s  %f %f %f", baseFrame.c_str(),
             x, y, rad2deg(yaw));
    }

    tf2::Vector3 linear = goalInBase.getOrigin();
    linear.setZ(0);
    double dist = linear.length();
    bool reverseWithoutTurning =
        (reverseWithoutTurningThreshold > dist && linear.x() < 0.0);

    if (!transformPose(frameId, baseFrame, goal, goalInBase)) {
        ROS_WARN("MoveBasic: Cannot determine robot pose for rotation");
        return;
    }

    if (dist > linearTolerance) {
        double requestedYaw = atan2(linear.y(), linear.x());
        if (reverseWithoutTurning) {
            if (requestedYaw > 0.0) {
                requestedYaw = -M_PI + requestedYaw;
            }
            else {
                requestedYaw = M_PI - requestedYaw;
            }
        }

        // Initial rotation
        if (std::abs(requestedYaw) > angularTolerance) {
            if (!rotate(requestedYaw, drivingFrame)) {
                return;
            }
        }
        sleep(localizationLatency);

        // Linear portion
        if (!moveLinear(goalInDriving, drivingFrame)) {
            return;
        }
        sleep(localizationLatency);

        // Final rotation
        if (std::abs(goalYaw - (yaw + requestedYaw)) > angularTolerance) {
            if (!rotate(goalYaw - (yaw + requestedYaw), drivingFrame)) {
                return;
            }
        }
    }

    actionServer->setSucceeded();
}



// Send a motion command

void MoveBasic::sendCmd(double angular, double linear)
{
   geometry_msgs::Twist msg;
   msg.angular.z = angular;
   msg.linear.x = linear;

   cmdPub.publish(msg);
}


// Main loop

void MoveBasic::run()
{
    ros::Rate r(20);

    while (ros::ok()) {
        ros::spinOnce();
        collision_checker->min_side_dist = minSideDist;
        forwardObstacleDist = collision_checker->obstacle_dist(true,
                                                               leftObstacleDist,
                                                               rightObstacleDist,
                                                               forwardLeft,
                                                               forwardRight);
        geometry_msgs::Vector3 msg;
        msg.x = forwardObstacleDist;
        msg.y = leftObstacleDist;
        msg.z = rightObstacleDist;
        obstacle_dist_pub.publish(msg);
        r.sleep();
    }
}


// Rotate relative to current orientation

bool MoveBasic::rotate(double yaw, const std::string& drivingFrame)
{
    tf2::Transform poseDriving;
    if (!getTransform(baseFrame, drivingFrame, poseDriving)) {
         abortGoal("MoveBasic: Cannot determine robot pose for rotation");
         return false;
    }

    double x, y, currentYaw;
    getPose(poseDriving, x, y, currentYaw);
    double requestedYaw = currentYaw + yaw;
    normalizeAngle(requestedYaw);
    ROS_INFO("MoveBasic: Requested rotation %f", rad2deg(requestedYaw));

    bool success = false;
    bool done = false;
    ros::Rate r(50);

    while (!done && ros::ok()) {
        ros::spinOnce();
        r.sleep();

        double x, y, currentYaw;
        tf2::Transform poseDriving;
        if (!getTransform(baseFrame, drivingFrame, poseDriving)) {
            abortGoal("MoveBasic: Cannot determine robot pose for rotation");
            return false;
        }
        getPose(poseDriving, x, y, currentYaw);

        double angleRemaining = requestedYaw - currentYaw;
        normalizeAngle(angleRemaining);

        double obstacle = collision_checker->obstacle_angle(angleRemaining > 0);
        double remaining = std::min(std::abs(angleRemaining), std::abs(obstacle));
        double velocity = std::max(minTurningVelocity,
            std::min(rotGain*remaining, std::min(maxTurningVelocity,
                    std::sqrt(2.0 * angularAcceleration *remaining))));

        if (actionServer->isPreemptRequested()) {
            ROS_INFO("MoveBasic: Stopping rotation due to preempt");
            done = true;
            success = false;
            velocity = 0;
        }

        if (std::abs(angleRemaining) < angularTolerance) {
            ROS_INFO("MoveBasic: Done rotation, error %f degrees", rad2deg(angleRemaining));
            velocity = 0;
            success = true;
            done = true;
        }

        bool reverse =  (angleRemaining < 0.0);
        if (reverse) {
            velocity = -velocity;
        }

        sendCmd(velocity, 0);
        ROS_DEBUG("Angle remaining: %f, Angular velocity: %f", rad2deg(angleRemaining), velocity);
    }
    return success;
}

// Move forward specified distance

bool MoveBasic::moveLinear(tf2::Transform& goalInDriving,
                           const std::string& drivingFrame)
{
    tf2::Transform poseDriving;
    if (!getTransform(drivingFrame, baseFrame, poseDriving)) {
         abortGoal("MoveBasic: Cannot determine robot pose for linear");
         return false;
    }

    tf2::Transform goalInBase = poseDriving * goalInDriving;
    tf2::Vector3 remaining = goalInBase.getOrigin();
    bool forward = (remaining.x() > 0);
    double requestedDistance = remaining.length();
    double prevDistRemaining = requestedDistance;
    bool pausingForObstacle = false;
    ros::Time last = ros::Time::now();
    ros::Time obstacleTime;
    ros::Duration abortTimeout(abortTimeoutSecs);

    // For lateral control
    double lateralIntegral = 0.0;
    double lateralError = 0.0;
    double prevLateralError = 0.0;
    double lateralDiff = 0.0;
    ros::Time sensorTime;

    bool success = false;
    bool done = false;
    ros::Rate r(50);
    while (!done && ros::ok()) {
        ros::spinOnce();
        r.sleep();

        if (!getTransform(drivingFrame, baseFrame, poseDriving)) {
             ROS_WARN("MoveBasic: Cannot determine robot pose for linear");
             return false;
        }
        goalInBase = poseDriving * goalInDriving;
        remaining = goalInBase.getOrigin();
        double distRemaining = sqrt(remaining.x() * remaining.x() + remaining.y() * remaining.y());

        // PID loop to control rotation to keep robot on path
        double rotation = 0.0;
        lateralDiff = lateralError - prevLateralError;
        lateralError = sideRecoverWeight * remaining.y();
        prevLateralError = lateralError;
        lateralIntegral += lateralError;
        rotation = (lateralKp * lateralError) + (lateralKi * lateralIntegral) +
                   (lateralKd * lateralDiff);
        // Clamp rotation
        rotation = std::max(-maxLateralVelocity, std::min(maxLateralVelocity,
                                                          rotation));
        ROS_DEBUG("MoveBasic: %f L %f, R %f %f %f %f %f \n",
                  forwardObstacleDist, leftObstacleDist, rightObstacleDist,
                  remaining.x(), remaining.y(), lateralError,
                  rotation);

        // Publish messages for PID tuning
        geometry_msgs::Vector3 pid_debug;
        pid_debug.x = remaining.x();
        pid_debug.y = lateralError;
        pid_debug.z = rotation;
        errorPub.publish(pid_debug);

        /* Collision Avoidance */

        double obstacleDist = forwardObstacleDist;
	if (requestedDistance < 0.0) {
		obstacleDist = collision_checker->obstacle_dist(false,
                                                        	leftObstacleDist,
                                                        	rightObstacleDist,
                                                        	forwardLeft,
                                                        	forwardRight);
	}

        double velocity = std::min(linGain*std::min(std::abs(obstacleDist), std::abs(distRemaining)),
                std::min(maxLinearVelocity, std::sqrt(2.0 * linearAcceleration * std::min(std::abs(obstacleDist), std::abs(distRemaining)))));

        bool obstacleDetected = (obstacleDist < forwardObstacleThreshold);
        if (obstacleDetected) {
            velocity = 0;
            if (!pausingForObstacle) {
                ROS_INFO("MoveBasic: PAUSING for OBSTACLE");
                obstacleTime = ros::Time::now();
                pausingForObstacle = true;
            }
            else {
                ROS_INFO("MoveBasic: Still waiting for obstacle at %f meters!", obstacleDist);
                ros::Duration waitTime = ros::Time::now() - obstacleTime;
                if (waitTime.toSec() > obstacleWaitThreshold) {
                    abortGoal("MoveBasic: Aborting due to obstacle");
                    success = false;
                    done = true;
                }
            }
        }

        if (!obstacleDetected && pausingForObstacle) {
            ROS_INFO("MoveBasic: Resuming after obstacle has gone");
            pausingForObstacle = false;
        }

        /* Abort Checks */

        if (actionServer->isPreemptRequested()) {
            ROS_INFO("MoveBasic: Stopping move due to preempt");
            velocity = 0;
            success = false;
            done = true;
        }

        if (distRemaining > prevDistRemaining) {
            prevDistRemaining = distRemaining;
            if (ros::Time::now() - last > abortTimeout) {
                abortGoal("MoveBasic: No progress towards goal for longer than timeout");
                velocity = 0;
                success = false;
                done = true;
            }
        }
	else {
	    last = ros::Time::now();
	}

        /* Finish Check */

        if ((std::abs(velocity) < velThreshold) && distRemaining < linearTolerance) {
            ROS_INFO("MoveBasic: Done linear, error: x: %f meters, y: %f meters", remaining.x(), remaining.y());
            velocity = 0;
            success = true;
            done = true;
        }

        if (!forward) {
            velocity = -velocity;
        }

        sendCmd(rotation, velocity);
        ROS_DEBUG("Distance remaining: %f, Linear velocity: %f", distRemaining, velocity);
    }
    return success;
}


int main(int argc, char ** argv) {
    ros::init(argc, argv, "move_basic");
    MoveBasic mb_node;
    mb_node.run();

    return 0;
}
