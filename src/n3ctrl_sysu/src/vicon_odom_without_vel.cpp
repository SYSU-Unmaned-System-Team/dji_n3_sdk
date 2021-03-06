//头文件
#include <ros/ros.h>
#include <iostream>
#include <Eigen/Eigen>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/TwistStamped.h>
#include <geometry_msgs/Quaternion.h>
#include <nav_msgs/Odometry.h>

using namespace std;

bool init_ok = false;
string object_name;
Eigen::Vector3d pos_now, pos_last, vel_now , att_rate;
geometry_msgs::Quaternion att_now;
Eigen::Vector3d vel_0, vel_1, vel_2, vel_3, vel_4, vel_filter;
nav_msgs::Odometry Drone_odom;

ros::Time time_now;
ros::Time time_pos_now, time_pos_last;

ros::Publisher odom_pub;

void mocap_cb(const geometry_msgs::PoseStamped::ConstPtr &msg)
{
    if ( !init_ok )
    {
        init_ok     = true;

        pos_now << msg->pose.position.x, msg->pose.position.y, msg->pose.position.z;
        att_now = msg->pose.orientation;
        pos_last = pos_now;
        time_pos_last = msg->header.stamp;

        vel_0 << 0.0, 0.0, 0.0;
        vel_1 << 0.0, 0.0, 0.0;
        vel_2 << 0.0, 0.0, 0.0;
        vel_3 << 0.0, 0.0, 0.0;
        vel_4 << 0.0, 0.0, 0.0;
        vel_filter << 0.0, 0.0, 0.0;
    }
    else
    {
        // 目前mocap坐标系为FRU坐标系，否则需要进行坐标转换
        time_pos_now = msg->header.stamp;

        pos_now << msg->pose.position.x, msg->pose.position.y, msg->pose.position.z;

        att_now = msg->pose.orientation;

        /** velocity filter **/
        double dt;

        // dt = ( time_pos_now - time_pos_last ).toSec( ); 
        
        dt = 0.02;

        // cout << "\033[1;32m" << " ---->dt, time:  " << dt << " [s]. " << "\033[0m" << endl;

        vel_0 = ( pos_now - pos_last ) /  dt;

        //vel_filter = ( vel_0 + vel_1 + vel_2 + vel_3 + vel_4 ) * 0.2;
        // vel_4 = vel_3;
        // vel_3 = vel_2;
        // vel_2 = vel_1;

        vel_filter = vel_0 ;
        
        // vel_1 = vel_0;
        //        cout << " time " << ( time_now - last_t ).toSec( ) << endl;
        // cout << " vel_filter " << vel_filter << "[m/s]" << std::endl;


        vel_now  = vel_filter;

        time_pos_last = time_pos_now;
        pos_last = pos_now;
    }
}

void mocap_vel_cb(const geometry_msgs::TwistStamped::ConstPtr &msg)
{
    Eigen::Vector3d vel_vicon;

    vel_vicon << msg->twist.linear.x, msg->twist.linear.y, msg->twist.linear.z;

    float error_vel = (vel_vicon - vel_now).norm();

    if( error_vel >  0.2 )
    {
        cout << "\033[1;31m" << " ----> wrong vel, error_vel:  " << error_vel << " [m]. " << "\033[0m" << endl;
    }
}

void timerCallback(const ros::TimerEvent& e)
{
    if (init_ok)
    {
        cout << " odom_pos [x y z]: " << pos_now.x() << " [ m ] " << pos_now.y() << " [ m ] " << pos_now.z() << " [ m ] " << endl;
        cout << " odom_vel [x y z]: " << vel_now.x() << " [m/s] " << vel_now.y() << " [m/s] " << vel_now.z() << " [m/s] " << endl;
    }
}

//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>主 函 数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
int main(int argc, char **argv)
{
    ros::init(argc, argv, "mocap_pos_to_odom");
    ros::NodeHandle nh("~");

    nh.param<string>("object_name", object_name, "dji_n3");

    // 通过vrpn_client_ros订阅来自动捕的消息（位置和姿态消息），并通过微分平滑滤波的方式估计线速度
    ros::Subscriber mocap_sub = nh.subscribe<geometry_msgs::PoseStamped>("/vrpn_client_node/"+ object_name + "/pose", 1, mocap_cb);
    // ros::Subscriber mocap_vel_sub = nh.subscribe<geometry_msgs::TwistStamped>("/vrpn_client_node/"+ object_name + "/twist", 1, mocap_vel_cb);

    // 将上述信息整合为里程计信息发布，用于位置环控制
    odom_pub = nh.advertise<nav_msgs::Odometry>("/dji_n3/odom", 10);

    ros::Timer timer = nh.createTimer(ros::Duration(10.0), timerCallback);

    // 频率
    ros::Rate rate(50.0);

    //固定的浮点显示
    cout.setf(ios::fixed);
    //setprecision(n) 设显示小数精度为n位
    cout<<setprecision(2);
    //左对齐
    cout.setf(ios::left);
    // 强制显示小数点
    cout.setf(ios::showpoint);
    // 强制显示符号
    cout.setf(ios::showpos);

    sleep(5.0);

    //>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>Main Loop<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
    while (ros::ok())
    {
        //回调一次 更新传感器状态
        ros::spinOnce();

        time_now = ros::Time::now();

        if( (time_now - time_pos_now).toSec() > 0.2 )
        {
            cout << "\033[1;31m" << " ----> mocap pos timeout, time delay:  " << (time_now - time_pos_now).toSec() << " [s]. " << "\033[0m" << endl;
        }

        Drone_odom.header.stamp            = time_now;
        Drone_odom.header.frame_id         = "world";
        Drone_odom.child_frame_id          = "world";
        Drone_odom.pose.pose.position.x    = pos_now.x();
        Drone_odom.pose.pose.position.y    = pos_now.y();
        Drone_odom.pose.pose.position.z    = pos_now.z();
        Drone_odom.pose.pose.orientation = att_now;
        Drone_odom.twist.twist.linear.x    = vel_now.x(); 
        Drone_odom.twist.twist.linear.y    = vel_now.y(); 
        Drone_odom.twist.twist.linear.z    = vel_now.z(); 
        Drone_odom.twist.twist.angular.x    = 0.0; 
        Drone_odom.twist.twist.angular.y    = 0.0; 
        Drone_odom.twist.twist.angular.z    = 0.0;
        if (init_ok)
        {
            odom_pub.publish(Drone_odom);
        }
        rate.sleep();
    }

    return 0;
}