#!/bin/bash
source /home/dji/dji_n3_sdk/devel/setup.bash
#roscore & sleep 1;

roslaunch djiros djiros.launch & sleep 5s;

roslaunch vrpn_client_ros sample.launch & sleep 1s;

roslaunch n3ctrl_sysu vicon_odom.launch & sleep 1s;

roslaunch n3ctrl ctrl_md.launch

# roslaunch ego_planner real.launch & sleep 2s

#rviz -d /home/dji/tfes/src/VINS-Fusion/config/vins_rviz_config.rviz

# rosbag record -a


