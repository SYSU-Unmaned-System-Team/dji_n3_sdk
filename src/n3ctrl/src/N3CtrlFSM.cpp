#include "N3CtrlFSM.h"
#include <uav_utils/converters.h>

using namespace Eigen;
using std::cout;
using std::endl;
using namespace uav_utils;

N3CtrlFSM::N3CtrlFSM(Parameter_t& param_, Controller& controller_, HovThrKF& hov_thr_kf_):
	param(param_), controller(controller_), hov_thr_kf(hov_thr_kf_)
{
	state = DIRECT_CTRL;
	axis_states[0] = FIX;
	axis_states[1] = FIX;
	axis_states[2] = FIX;
	axis_states[3] = FIX;
	work_mode = REALTIME;
	idling_state = NOIDLING;

	hover_pose.setZero();
	last_command_id = 0;

	// 初始化point_data
	// z轴设置的小一点
	point_data.p(0) = 0.0;
	point_data.p(1) = 0.0;
	point_data.p(2) = 0.5;
	point_data.yaw = 0.0;

	point_data.get_cmd = false;
}

void N3CtrlFSM::process()
{
	ros::Time now_time = ros::Time::now();
	// 本函数的实际执行频率，ctrl_rate = 100，即100Hz
	if ((now_time - last_ctrl_time).toSec() < (1.0 / param.ctrl_rate))
	{
		return;
	}
	last_ctrl_time = now_time;

	dbgss.str("");
	dbgss << "now time: " << now_time << endl;

	// ROS_WARN_STREAM(boost::format("curr: %.3f odom: %.3f dt=%.3f")%
	// 			now_time.toSec() %
	// 			odom_data.msg.header.stamp.toSec() %
	// 			(now_time-odom_data.msg.header.stamp).toSec());
	// ROS_INFO_STREAM(boost::format("rcv : %.3f odom: %.3f dt=%.3f")%
	// 			odom_data.rcv_stamp.toSec() %
	// 			odom_data.msg.header.stamp.toSec() %
	// 			(odom_data.rcv_stamp-odom_data.msg.header.stamp).toSec());

	// -------- for simulation ----------
	// 此处不理会
	if (work_mode == SIMULATION || work_mode == SIM_WITHOUT_RC)
	{
		rc_data.rcv_stamp = now_time;
		rc_data.is_api_mode = true;
		rc_data.is_command_mode = true;
		if (state == DIRECT_CTRL)
		{
			rc_data.enter_api_mode = true;
			rc_data.enter_command_mode = true;
		}
		else if (state == JS_CTRL)
		{
			rc_data.enter_api_mode = false;
			if (work_mode == SIM_WITHOUT_RC) {
				rc_data.enter_command_mode = true;
			}
			else {
				rc_data.enter_command_mode = false;
			}
		}
		else if (state == CMD_HOVER || state == CMD_CTRL) {
			if (work_mode == SIM_WITHOUT_RC) {
				rc_data.enter_command_mode = false;
			}
		}

		imu_data.q = odom_data.q;
		imu_data.rcv_stamp = now_time;
	}
	// ----------------------------------

	// 检测遥控器是否一直连接
	if (!rc_is_received(now_time))
	{
		ROS_ERROR("RC lost for %3f seconds!!!! Exit...", (now_time - rc_data.rcv_stamp).toSec());
		dbgss << "RC lost for "
		      << (now_time - rc_data.rcv_stamp).toSec()
		      << " seconds!!!! EXIT......";
		std_msgs::Header dbgmsg;
		dbgmsg.stamp = ros::Time::now();
		dbgmsg.frame_id = std::string(dbgss.str());
		fsm_dbg_pub.publish(dbgmsg);
		ROS_ASSERT(false && "RC LOST AND EXIT");
		exit(-1);
	}

	// enter_command_mode来自脚架通道，即进入自动指令模式
	if (rc_data.enter_command_mode)
	{
		geometry_msgs::PoseStamped msg;
		msg.header.stamp = ros::Time::now();
		traj_start_trigger_pub.publish(msg);
		ROS_INFO("[N3CTRL] Traj trigger signal sent.");
		if (state == DIRECT_CTRL)
		{
			rc_data.check_enter_command_mode();
			ROS_WARN("[N3ctrl] IN DIRECT, Sending TRIGGER!");
		}
	}

	// 只针对 (state == JS_CTRL || state == CMD_CTRL || state == DIRECT_CTRL)三种状态
	// 函数定义在N3CtrlFSM_state.cpp
	// 暂不清楚，看着像是根据当前情况判断是否进入怠速旋转的作用
	determine_idling(now_time);
	// 确定状态
	// 函数定义在N3CtrlFSM_state.cpp
	// 功能包括:判断odom的坐标、odom是否超时、判断cmd是否valid
	// 最重要的是判断当前的状态机，并重置控制参数
	determine_state(now_time);
	// 根据状态执行控制
	// 函数定义在N3CtrlFSM_control.cpp
	process_control(now_time);

	// 状态打印
    if(printf_count > 200)
	{
		printf_state(now_time);
		printf_count = 0;
	}else
	{
		printf_count++;
	}


	if (state == CMD_HOVER || state == CMD_CTRL)
	{
		dbgss << "CmdID: last[" << last_command_id << "] rcv["
		      << cmd_data.trajectory_id << "]" << endl;
	}

	if (state == JS_CTRL || state == CMD_HOVER)
	{
		dbgss << "hov: " << hover_pose.transpose() << endl;
		dbgss << "cur: " << odom_data.p.transpose() << endl;
	}

	// 不清楚这块publish_led_vis是如何作用的
	if (state == JS_NO_CTRL || state == JS_RESET_POS_CTRL || state == CMD_NO_CTRL || state == CMD_RESET_POS_CTRL)
	{
		stateVisualizer.publish_led_vis(now_time, "noodom");
	}
	else if (state == DIRECT_CTRL)
	{
		if (idling_state==NOIDLING)
			stateVisualizer.publish_led_vis(now_time, "direct");
		else
			stateVisualizer.publish_led_vis(now_time, "idledirect");
	}
	else if (state == JS_CTRL)
	{
		if (idling_state==NOIDLING)
			stateVisualizer.publish_led_vis(now_time, "js");
		else
			stateVisualizer.publish_led_vis(now_time, "idlejs");
	}
	else if (state == CMD_HOVER)
	{
		stateVisualizer.publish_led_vis(now_time, "ctrlhov");
	}
	else if (state == CMD_CTRL)
	{
		stateVisualizer.publish_led_vis(now_time, "ctrlcmd");
	}
	else
	{
		stateVisualizer.publish_led_vis(now_time, "null");
	}

	std_msgs::Header dbgmsg;
	dbgmsg.stamp = ros::Time::now();
	dbgmsg.frame_id = std::string(dbgss.str());
	fsm_dbg_pub.publish(dbgmsg);
}

void N3CtrlFSM::printf_state(const ros::Time& now_time)
{
    cout << GREEN <<">>>>>>>>>>>>>>>>>>>>>>>> DJI N3Ctrl State  <<<<<<<<<<<<<<<<<<<<<<<<<<<"<< TAIL  <<endl;
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

	if (state == DIRECT_CTRL)
	{
		cout << GREEN  << " [DIRECT_CTRL] "<< TAIL <<endl;
	}	
	else if (state == JS_CTRL)
	{
		cout << GREEN  << " [JS_CTRL] "<< TAIL <<endl;
	}
	else if (state == JS_NO_CTRL)
	{
		cout << GREEN  << " [JS_NO_CTRL] "<< TAIL <<endl;
	}
	else if (state == JS_RESET_POS_CTRL)
	{
		cout << GREEN  << " [JS_RESET_POS_CTRL] "<< TAIL <<endl;
	}
	else if (state == CMD_HOVER)
	{
		cout << GREEN  << " [CMD_HOVER] "<< TAIL <<endl;
	}
	else if (state == CMD_CTRL)
	{
		cout << GREEN  << " [CMD_CTRL] "<< TAIL <<endl;
	}
	else if (state == CMD_NO_CTRL)
	{
		cout << GREEN  << " [CMD_NO_CTRL] "<< TAIL <<endl;
	}
	else if (state == CMD_RESET_POS_CTRL)
	{
		cout << GREEN  << " [CMD_RESET_POS_CTRL] "<< TAIL <<endl;
	}
	else
	{
		cout << GREEN  << " [Wrong mode] "<< TAIL <<endl;
	}


	if(point_data.get_cmd)
	{
    	cout << GREEN  << "pos_cmd [X Y Z] : " << point_data.p(0) << " [ m ] "<< point_data.p(1) <<" [ m ] "<< point_data.p(2) <<" [ m ] "<< TAIL <<endl;
	}

	cout << BLUE  << "N3 State: "<< TAIL <<endl;
    cout << BLUE  << "pos [X Y Z] : " << odom_data.p(0) << " [ m ] "<< odom_data.p(1) <<" [ m ] "<< odom_data.p(2) <<" [ m ] "<< TAIL <<endl;
    cout << BLUE  << "vel [X Y Z] : " << odom_data.v(0) << " [m/s] "<< odom_data.v(1) <<" [m/s] "<< odom_data.v(2) <<" [m/s] "<< TAIL <<endl;
	cout << BLUE  << "RC State: "<< TAIL <<endl;
	cout << BLUE  << "Main channel [roll,pitch,yaw,thr] : [" << rc_data.roll << "， "<< rc_data.pitch << "， "<< rc_data.yaw << "， "<< rc_data.thr << "] " << TAIL <<endl;
	cout << BLUE  << "Aux chaneel [mode,gear] : [" << rc_data.mode << "， "<< rc_data.gear << "] "<< TAIL <<endl;
	cout << BLUE  << "Controller State: "<< TAIL <<endl;
	cout << BLUE  << "low level cmd [roll,pitch,yaw,thr] : [" << controller.control_u.roll << "， "<< controller.control_u.pitch << "， "<< controller.control_u.yaw << "， "<< controller.control_u.thrust<< " ]"<< TAIL <<endl;
	cout << BLUE  << "low level cmd [mode,yaw_mode]: [" << controller.control_u.mode << "， "<< controller.control_u.yaw_mode << "] "<< TAIL <<endl;
	// full_thrust = mass * gra / hov_thr_kf.get_hov_thr()
	// u.thrust = u1 / param.full_thrust;
	cout << BLUE  << "Hover (full thrust): " << param.mass * param.gra / hov_thr_kf.get_hov_thr() << TAIL <<endl;
	

}

bool N3CtrlFSM::rc_is_received(const ros::Time& now_time)
{
	return (now_time - rc_data.rcv_stamp).toSec() < param.msg_timeout.rc;
}


// ---- tools ----
double N3CtrlFSM::get_yaw_from_odom()
{
	return get_yaw_from_quaternion(odom_data.q);
}

void N3CtrlFSM::get_des_from_js(Vector3d& des_v, double& dyaw)
{
	Vector3d v_c(
	    rc_data.pitch * param.rc.hori_velo_scale,
	    -rc_data.roll * param.rc.hori_velo_scale,
	    rc_data.thr   * param.rc.vert_velo_scale);

	// assume that protocal's q is b^q_g.
	Matrix3d wRc = rotz(get_yaw_from_odom());
	Vector3d v_w = wRc * v_c;

	des_v = v_w;

	// yaw axis from rc is towards ground
	dyaw = -rc_data.yaw * param.rc.yaw_scale;
}

void N3CtrlFSM::align_with_imu(Controller_Output_t& u)
{
	double imu_yaw = get_yaw_from_quaternion(imu_data.q); //will never be used. zxzxzxzx
	double odom_yaw = get_yaw_from_odom();
	double des_yaw = u.yaw;
	//u.yaw = yaw_add(des_yaw, -odom_yaw) * 3.0f; //zxzxzxzx
	u.yaw = yaw_add(yaw_add(des_yaw, -odom_yaw), imu_yaw); //zxzxzxzx
	//u.yaw = -20.0 / 180 * 3.14;
	//ROS_WARN("imu_yaw=%f odom_yaw=%f des_yaw=%f u.yaw=%f",imu_yaw,odom_yaw,des_yaw,u.yaw); //zxzxzxzx
	// printf("des:%.3f \todom:%.3f \timu:%.3f \trst:%.3f\n",
	// 	des_yaw, odom_yaw, imu_yaw, u.yaw);
	// u.yaw = yaw_add(des_yaw, -odom_yaw) * 3.0;
	// u.yaw = toRad(12.0);
};

void N3CtrlFSM::set_hov_with_odom()
{
	hover_pose.head<3>() = odom_data.p;
	hover_pose(3) = get_yaw_from_odom();
	// ROS_INFO("hov@<%.2f,%.2f,%.2f,%.2f>",
	// 	hover_pose(0), hover_pose(1), hover_pose(2), toDeg(hover_pose(3)));
	dbgss << "hov@< " << hover_pose.transpose() << " >" << endl;
}

void N3CtrlFSM::flush_last_command_id()
{
	last_command_id = cmd_data.trajectory_id;
}

std::string N3CtrlFSM::state_to_str()
{
	switch (state)
	{
	case DIRECT_CTRL		: return std::string("DIRECT");
	case JS_CTRL 			: return std::string("JS-CTRL");
	case JS_NO_CTRL 		: return std::string("JS-NO");
	case JS_RESET_POS_CTRL 	: return std::string("JS-LC");
	case CMD_HOVER 			: return std::string("CMD-HOV");
	case CMD_CTRL 			: return std::string("CMD-CTRL");
	case CMD_NO_CTRL 		: return std::string("CMD-NO");
	case CMD_RESET_POS_CTRL : return std::string("CMD-LC");
	default: ROS_ASSERT(false);
	}
	return std::string();
}

std::string N3CtrlFSM::axis_state_to_str()
{
	std::string s;
	for (int i = 0; i < 4; ++i)
	{
		switch (axis_states[i])
		{
		case FIX 	: s += std::string("F"); break;
		case MOVE 	: s += std::string("M"); break;
		case BREAK	: s += std::string("B"); break;
		default: ROS_ASSERT(false);
		}
	}
	return s;
}

void N3CtrlFSM::set_work_mode(Work_Mode_t mode)
{
	work_mode = mode;
	if (work_mode == SIMULATION)
	{
		ROS_INFO("[N3CTRL] work mode set to SIMULATION");
	}
	else if (work_mode == SIM_WITHOUT_RC) {
		ROS_INFO("[N3CTRL] work mode set to SIM_WITHOUT_RC");
	}
	else if (work_mode == REALTIME)
	{
		ROS_INFO("[N3CTRL] work mode set to REALTIME");
	}
	else
	{
		ROS_ASSERT(false && "INVALID LOGIC BRANCH.");
	}
}

void N3CtrlFSM::publish_desire(const Desired_State_t& des)
{
	geometry_msgs::PoseStamped msg;
	msg.header = odom_data.msg.header;

	msg.pose.position.x = des.p(0);
	msg.pose.position.y = des.p(1);
	msg.pose.position.z = des.p(2);

	Eigen::Quaterniond q = yaw_to_quaternion(des.yaw);

	msg.pose.orientation.w = q.w();
	msg.pose.orientation.x = q.x();
	msg.pose.orientation.y = q.y();
	msg.pose.orientation.z = q.z();

	des_pose_pub.publish(msg);
}

void N3CtrlFSM::set_js_ctrl_mode(JS_CTRL_Mode_t mode)
{
	js_ctrl_mode = mode;
	if (js_ctrl_mode == JS_CTRL_MODE_RAW)
	{
		ROS_INFO("[N3CTRL] js_ctrl_mode set to JS_CTRL_MODE_RAW");
	}
	else if (js_ctrl_mode == JS_CTRL_MODE_FEEDBACK)
	{
		ROS_INFO("[N3CTRL] js_ctrl_mode set to JS_CTRL_MODE_FEEDBACK");
	}
	else
	{
		ROS_ASSERT(false && "INVALID LOGIC BRANCH.");
	}
}
