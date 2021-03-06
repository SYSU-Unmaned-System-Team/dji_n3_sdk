#include "controller.h"
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <uav_utils/converters.h>
#include <geometry_msgs/Vector3Stamped.h>
#include <geometry_msgs/QuaternionStamped.h>
#include <geometry_msgs/WrenchStamped.h>
#include <boost/format.hpp>
#include <n3ctrl/ControllerDebug.h>

using namespace Eigen;
using std::cout;
using std::endl;
using namespace uav_utils;

Controller::Controller(Parameter_t& param_):
	param(param_)
{
	is_configured = false;
	int_e_v.setZero();
}

void Controller::config()
{
	config_gain(param.hover_gain);
	is_configured = true;
}

void Controller::config_gain(const Parameter_t::Gain& gain)
{
	Kp.setZero();
	Kv.setZero();
	Ka.setZero();
	// CMD_CTRL时使用track参数，其他均使用hover参数
	Kp(0,0) = gain.Kp0;
	Kp(1,1) = gain.Kp1;
	Kp(2,2) = gain.Kp2;
	Kv(0,0) = gain.Kv0;
	Kv(1,1) = gain.Kv1;
	Kv(2,2) = gain.Kv2;
	Kvi(0,0) = gain.Kvi0;
	Kvi(1,1) = gain.Kvi1;
	Kvi(2,2) = gain.Kvi2;
	Ka(0,0) = gain.Ka0;
	Ka(1,1) = gain.Ka1;
	Ka(2,2) = gain.Ka2;
	Kyaw = gain.Kyaw;
	tilt_angle_max = gain.tilt_angle_max;
}

void Controller::update(
	const Desired_State_t& des, 
	const Odom_Data_t& odom, 
	Controller_Output_t& u, 
	SO3_Controller_Output_t& u_so3
)
{
	ROS_ASSERT_MSG(is_configured, "Gains for controller might not be initialized!");
	std::string constraint_info("");
	// 误差项，期望力
	Vector3d e_p, e_v, F_des;
	double e_yaw = 0.0;


	if (des.v(0) != 0.0 || des.v(1) != 0.0 || des.v(2) != 0.0) {
		ROS_INFO("Reset integration");
		int_e_v.setZero();
	}

	// 获取当前偏航角
	double yaw_curr = get_yaw_from_quaternion(odom.q);
	// 期望偏航角
	double	yaw_des = des.yaw;
	// 坐标转换矩阵（仅根据当前yaw角生成）
	// 没看懂为什么要乘上wRc和cRw
	// 
	Matrix3d wRc = rotz(yaw_curr);
	Matrix3d cRw = wRc.transpose();

	// 位置误差
	e_p = des.p - odom.p;
	// p控制
	// e_p是ENU坐标系的,u_p也是ENU系的
	//  wRc * Kp * cRw * e_p =  Kp * e_p
	Eigen::Vector3d u_p = wRc * Kp * cRw * e_p;

	// 相当于期望速度限制幅度

	for (size_t k = 0; k < 3; ++k) 
	{
		if(u_p(k) > 0.5)
		{
			u_p(k) = 0.5;
		}else if(u_p(k) < -0.5)
		{
			u_p(k) = -0.5;
		}
	}


	
	// 速度误差 = 期望速度  + 位置p控制 - 当前速度
	// des.v = 0.0 时，等同于串级控制的意思
	e_v = des.v + u_p - odom.v;
		
	// 积分项？
	for (size_t k = 0; k < 3; ++k) 
	{
		if (std::fabs(e_v(k)) < 1.0) 
		{
			int_e_v(k) += e_v(k) * 1.0 / 100.0;
		}
	}

	// 速度比例控制
	Eigen::Vector3d u_v_p = wRc * Kv * cRw * e_v;

	// 速度积分控制
	Eigen::Vector3d u_v_i = wRc * Kvi * cRw * int_e_v;

	const std::vector<double> integration_output_limits = {0.4, 0.4, 0.4};
	for (size_t k = 0; k < 3; ++k) {
		if (std::fabs(u_v_i(k)) > integration_output_limits[k]) {
			uav_utils::limit_range(u_v_i(k), integration_output_limits[k]);
			ROS_INFO("Integration saturate for axis %zu, value=%.3f", k, u_v_i(k));
		}
	}

	// 速度控制
	Eigen::Vector3d u_v = u_v_p + u_v_i;

	// 偏航角误差
	e_yaw = yaw_des - yaw_curr;

	while(e_yaw > M_PI) e_yaw -= (2 * M_PI);
	while(e_yaw < -M_PI) e_yaw += (2 * M_PI);

	// 偏航教控制量
	double u_yaw = Kyaw * e_yaw;
	
	// 期望力 = 质量*控制量 + 重力抵消 + 期望加速度*质量*Ka
	F_des = u_v * param.mass + Vector3d(0, 0, param.mass * param.gra) + Ka * param.mass * des.a;
	
	// 如果向上推力小于重力的一半
	// 或者向上推力大于重力的两倍
	if (F_des(2) < 0.5 * param.mass * param.gra)
	{
		constraint_info = boost::str(
			boost::format("thrust too low F_des(2)=%.3f; ")% F_des(2));
		F_des = F_des / F_des(2) * (0.5 * param.mass * param.gra);
	}
	else if (F_des(2) > 2 * param.mass * param.gra)
	{
		constraint_info = boost::str(
			boost::format("thrust too high F_des(2)=%.3f; ")% F_des(2));
		F_des = F_des / F_des(2) * (2 * param.mass * param.gra);
	}

	// 角度限制幅度
	if (std::fabs(F_des(0)/F_des(2)) > std::tan(toRad(tilt_angle_max)))
	{
		constraint_info += boost::str(boost::format("x(%f) too tilt; ")
			% toDeg(std::atan2(F_des(0),F_des(2))));
		F_des(0) = F_des(0)/std::fabs(F_des(0)) * F_des(2) * std::tan(toRad(tilt_angle_max));
	}

	// 角度限制幅度
	if (std::fabs(F_des(1)/F_des(2)) > std::tan(toRad(tilt_angle_max)))
	{
		constraint_info += boost::str(boost::format("y(%f) too tilt; ")
			% toDeg(std::atan2(F_des(1),F_des(2))));
		F_des(1) = F_des(1)/std::fabs(F_des(1)) * F_des(2) * std::tan(toRad(tilt_angle_max));	
	}
	// }

	// pub_debug_msgs: false ,此处无用
	if(param.pub_debug_msgs)
	{
		std_msgs::Header msg;
		msg = odom.msg.header;

		std::stringstream ss;

		if (constraint_info=="") constraint_info = "constraint no effect";
		ss << std::endl << constraint_info << std::endl;
		ss << "ep0 " << e_p(0) << " | ";
		ss << "ep1 " << e_p(1) << " | ";
		ss << "ep2 " << e_p(2) << " | ";
		ss << "ev0 " << e_v(0) << " | ";
		ss << "ev1 " << e_v(1) << " | ";
		ss << "ev2 " << e_v(2) << " | ";
		ss << "Fdes0 " << F_des(0) << " | ";
		ss << "Fdes1 " << F_des(1) << " | ";
		ss << "Fdes2 " << F_des(2) << " | ";

		msg.frame_id = ss.str();
		ctrl_dbg_pub.publish(msg);

		geometry_msgs::Vector3Stamped m;
		Vector3d d;
		
		m.header = odom.msg.header;

		d = -Kp * cRw * e_p;
		// m.vector.x = d(0);
		// m.vector.y = d(1);
		// m.vector.z = d(2);

		m.vector.x = e_yaw;
		m.vector.y = yaw_des;
		m.vector.z = yaw_curr;

		ctrl_dbg_p_pub.publish(m);

		d = -Kv * cRw * e_v;
		m.vector.x = d(0);
		m.vector.y = d(1);
		m.vector.z = d(2);
		ctrl_dbg_v_pub.publish(m);
		
		d = param.mass * des.a;
		m.vector.x = d(0);
		m.vector.y = d(1);
		m.vector.z = d(2);
		ctrl_dbg_a_pub.publish(m);

		n3ctrl::ControllerDebug dbg_msg;
		dbg_msg.header = odom.msg.header;
		dbg_msg.des_p = uav_utils::to_vector3_msg(des.p);
		dbg_msg.u_p_p = uav_utils::to_vector3_msg(u_p);
		dbg_msg.u_p_i = uav_utils::to_vector3_msg(Eigen::Vector3d::Zero());
		dbg_msg.u_p = uav_utils::to_vector3_msg(u_p);
		dbg_msg.des_v = uav_utils::to_vector3_msg(des.v);
		dbg_msg.u_v_p = uav_utils::to_vector3_msg(u_v_p);
		dbg_msg.u_v_i = uav_utils::to_vector3_msg(u_v_i);
		dbg_msg.u_v = uav_utils::to_vector3_msg(u_v);

		dbg_msg.k_p_p = uav_utils::to_vector3_msg(Kp.diagonal());
		dbg_msg.k_p_i = uav_utils::to_vector3_msg(Eigen::Vector3d::Zero());
		dbg_msg.k_v_p = uav_utils::to_vector3_msg(Kv.diagonal());
		dbg_msg.k_v_i = uav_utils::to_vector3_msg(Kvi.diagonal());

		ctrl_val_dbg_pub.publish(dbg_msg);
	}

	Vector3d z_b_des = F_des / F_des.norm();
	
	/////////////////////////////////////////////////
	// Z-X-Y Rotation Sequence                
	// Vector3d x_c_des = Vector3d(std::cos(yaw_des), sin(yaw_des), 0.0);
	// Vector3d y_b_des = z_b_des.cross(x_c_des) / z_b_des.cross(x_c_des).norm();
	// Vector3d x_b_des = y_b_des.cross(z_b_des);
	/////////////////////////////////////////////////

	/////////////////////////////////////////////////
	// Z-Y-X Rotation Sequence                
	Vector3d y_c_des = Vector3d(-std::sin(yaw_des), std::cos(yaw_des), 0.0);
	Vector3d x_b_des = y_c_des.cross(z_b_des) / y_c_des.cross(z_b_des).norm();
	Vector3d y_b_des = z_b_des.cross(x_b_des);
	///////////////////////////////////////////////// 

	Matrix3d R_des1; // it's wRb
	R_des1 << x_b_des, y_b_des, z_b_des;
	
	Matrix3d R_des2; // it's wRb
	R_des2 << -x_b_des, -y_b_des, z_b_des;
	
	Vector3d e1 = R_to_ypr(R_des1.transpose() * odom.q.toRotationMatrix());
	Vector3d e2 = R_to_ypr(R_des2.transpose() * odom.q.toRotationMatrix());

	Matrix3d R_des; // it's wRb

	if (e1.norm() < e2.norm())
	{
		R_des = R_des1;
	}
	else
	{
		R_des = R_des2;
	}

	// so3 control, 此处被屏蔽
	// {	// so3 control
	// 	u_so3.Rdes = R_des;
	// 	u_so3.Fdes = F_des;

	// 	Matrix3d wRb_odom = odom.q.toRotationMatrix();
	// 	Vector3d z_b_curr = wRb_odom.col(2);
	// 	u_so3.net_force = F_des.dot(z_b_curr);
	// }

	{	// n3 api control in forward-left-up frame
		// F_des是位于ENU坐标系的,F_c是FLU
		Vector3d F_c = wRc.transpose() * F_des;
		// 无人机姿态的矩阵形式
		Matrix3d wRb_odom = odom.q.toRotationMatrix();
		// 第三列
		Vector3d z_b_curr = wRb_odom.col(2);
		// 机体系下的推力合力 相当于Rb * F_enu 惯性系到机体系
		double u1 = F_des.dot(z_b_curr);
		double fx = F_c(0);
		double fy = F_c(1);
		double fz = F_c(2);
		// 期望roll， rad
		u.roll  = std::atan2(-fy, fz);
		u.pitch = std::atan2( fx, fz);
		// 油门 = 期望推力/最大推力
		// full_thrust = mass * gra / hov_percent;
		// full_thrust会根据hover_kf更新
		// 所以不适合直接起飞
		// 这里相当于认为油门是线性的,满足某种比例关系,即认为某个重量 = 悬停油门
		// 悬停油门与电机参数有关系,也取决于质量
		u.thrust = u1 / param.full_thrust;
		// VERT_THRU = 1.0;
		u.mode = Controller_Output_t::VERT_THRU;
		// use_yaw_rate_ctrl = false
		if(param.use_yaw_rate_ctrl){
			u.yaw_mode = Controller_Output_t::CTRL_YAW_RATE;
			u.yaw = u_yaw;
		}
		else{
			u.yaw_mode = Controller_Output_t::CTRL_YAW;
			u.yaw = des.yaw;
		}
		
		// cout << "----------" << endl;
		// cout << z_b_curr.transpose() << endl;
		// cout << F_c.transpose() << endl;
		// cout << u1 << endl;
	}

	if(param.pub_debug_msgs)
	{
		Vector3d ypr_des = R_to_ypr(R_des);
		Vector3d ypr_real = R_to_ypr(odom.q.toRotationMatrix());
		geometry_msgs::Vector3Stamped m;
		m.header = odom.msg.header;
		m.vector.x = ypr_des(2);
		m.vector.y = ypr_des(1);
		m.vector.z = ypr_des(0);
		ctrl_dbg_att_des_pub.publish(m);
		m.header = odom.msg.header;
		m.vector.x = ypr_real(2);
		m.vector.y = ypr_real(1);
		m.vector.z = ypr_real(0);
		ctrl_dbg_att_real_pub.publish(m);
	
		output_visualization(u);
	}

};

void Controller::publish_ctrl(const Controller_Output_t& u, const ros::Time& stamp, const ros::Time& extra_stamp)
{
	// 存储
	control_u = u;

	sensor_msgs::Joy msg;

	msg.header.stamp = stamp;
	msg.header.frame_id = std::string("FRD");

	// need to translate to forward-right-down frame
	// u.roll u.pitch 的单位是rad，转换为了deg
	msg.axes.push_back(toDeg(u.roll));
	msg.axes.push_back(toDeg(-u.pitch));

	// if mode > 0, thrust = 0~100%;
	// if mode < 0, thrust = -? m/s ~ +? m/s
	if (u.mode < 0)
	{
		msg.axes.push_back(u.thrust);
	}
	else
	{
		msg.axes.push_back(u.thrust*100);	
	}
	//  u.yaw 的单位是rad，转换为了deg
	msg.axes.push_back(toDeg(-u.yaw));
	// 标志位
	msg.axes.push_back(u.mode);
	msg.axes.push_back(u.yaw_mode);

	//add time stamp for debug
	// 无实际意义，用于debug
    msg.buttons.push_back(100000);
    msg.buttons.push_back(extra_stamp.sec/msg.buttons[0]);
    msg.buttons.push_back(extra_stamp.sec%msg.buttons[0]);
    msg.buttons.push_back(extra_stamp.nsec/msg.buttons[0]);
    msg.buttons.push_back(extra_stamp.nsec%msg.buttons[0]);
	
    ctrl_pub.publish(msg);
}

void Controller::publish_so3_ctrl(const SO3_Controller_Output_t& u_so3, const ros::Time& stamp)
{
	//Eigen::Vector3d T_w = u_so3.Fdes;
	Eigen::Quaterniond q(u_so3.Rdes);

	geometry_msgs::QuaternionStamped att_msg;

	att_msg.header.stamp = stamp;
	att_msg.header.frame_id = std::string("world");
	att_msg.quaternion.x = q.x();
	att_msg.quaternion.y = q.y();
	att_msg.quaternion.z = q.z();
	att_msg.quaternion.w = q.w();

	ctrl_so3_attitude_pub.publish(att_msg);

	geometry_msgs::WrenchStamped thr_msg;

	thr_msg.header.stamp = stamp;
	thr_msg.header.frame_id = std::string("body");
	thr_msg.wrench.force.z = u_so3.net_force / param.full_thrust;

	ctrl_so3_thrust_pub.publish(thr_msg);

	// quadrotor_msgs::SO3Command msg;
	// msg.header.stamp = stamp;
	// msg.header.frame_id = std::string("body");

	// msg.force.x = T_w(0);
	// msg.force.y = T_w(1);
	// msg.force.z = T_w(2);

	// msg.orientation.x = q.x();
	// msg.orientation.y = q.y();
	// msg.orientation.z = q.z();
	// msg.orientation.w = q.w();

	// msg.kR[0] = Kr(0,0);
	// msg.kR[1] = Kr(1,1);
	// msg.kR[2] = Kr(2,2);

	// msg.kOm[0] = Kw(0,0);
	// msg.kOm[1] = Kw(1,1);
	// msg.kOm[2] = Kw(2,2);

	// msg.aux.kf_correction = 0.0;
	// msg.aux.angle_corrections[0] = 0.0;
	// msg.aux.angle_corrections[1] = 0.0;
	// msg.aux.enable_motors = true;
	// msg.aux.use_external_yaw = false;

	// ctrl_so3_pub.publish(msg);
}

void Controller::output_visualization(const Controller_Output_t& u)
{
	double fn = u.thrust;
	double tan_r = std::tan(u.roll);
	double tan_p = std::tan(u.pitch);
	double fz = std::sqrt(fn*fn/(tan_r*tan_r+tan_p*tan_p+1));
	double fx = fz * tan_p;
	double fy = -fz * tan_r;

	sensor_msgs::Imu msg;
	msg.header.stamp = ros::Time::now();
	msg.header.frame_id = std::string("intermediate");
	msg.linear_acceleration.x = fx;
	msg.linear_acceleration.y = fy;
	msg.linear_acceleration.z = fz;

	ctrl_vis_pub.publish(msg);
}

float Controller::constrain_function(float data, float Max)
{
    if(abs(data)>Max)
    {
        return (data > 0) ? Max : -Max;
    }
    else
    {
        return data;
    }
}

void Controller::pos_controller(
	const Desired_State_t& des, 
	const Odom_Data_t& odom, 
	Controller_Output_t& u)
{
	if(!is_configured)
	{
		ROS_INFO("\033[1;31m---->Gains for controller might not be initialized!\033[0m");
	}

	// 获取当前偏航角
	double yaw_curr = get_yaw_from_quaternion(odom.q);
	// 期望偏航角
	double	yaw_des = des.yaw;
	// 坐标转换矩阵（仅根据当前yaw角生成）
	// 没看懂为什么要乘上wRc和cRw
	// 
	Matrix3d wRc = rotz(yaw_curr);
	Matrix3d cRw = wRc.transpose();

	// 误差项，期望力
	Vector3d e_p, e_v, F_des;
	double e_yaw = 0.0;

	e_p[0] = des.p[0] - odom.p[0];
	e_p[1] = des.p[1] - odom.p[1];
	e_p[2] = des.p[2] - odom.p[2];

	e_v[0] = des.v[0] - odom.v[0];
	e_v[1] = des.v[1] - odom.v[1];
	e_v[2] = des.v[2] - odom.v[2];

    // 误差项限幅
    for (int i=0; i<3; i++)
    {
        e_p[i] = constrain_function(e_p[i], 1.0);
        e_v[i] = constrain_function(e_v[i], 1.0);
    }

	Eigen::Vector3d accel_sp;
    for (int i=0; i<3; i++)
    {
        accel_sp[i] = Kp(i,i) * e_p[i] + Kv(i,i) * e_v[i] + Kvi(i,i) *int_e_v[i];
    }

    for (int i=0; i<3; i++)
    {
        if(abs(e_p[i]) < 0.5)
        {
            int_e_v[i] += e_p[i] * 0.01;

            if(abs(int_e_v[i]) > 2.0)
            {
                cout << "Integral saturation! " << " [0-1-2] "<< i <<endl;
                cout << "[integral]: "<< int_e_v[i]<<" [int_max]: "<<2.0<<" [m/s] "<<endl;
            }

            int_e_v[i] = constrain_function(int_e_v[i], 2.0);
        }else
        {
            int_e_v[i] = 0;
        }
    }

	// 偏航角误差
	e_yaw = yaw_des - yaw_curr;

	while(e_yaw > M_PI) e_yaw -= (2 * M_PI);
	while(e_yaw < -M_PI) e_yaw += (2 * M_PI);

	// 偏航教控制量
	double u_yaw = Kyaw * e_yaw;
	
	// 期望力 = 质量*控制量 + 重力抵消 + 期望加速度*质量*Ka
	F_des = accel_sp * param.mass + Vector3d(0, 0, param.mass * param.gra);
	
	// 如果向上推力小于重力的一半
	// 或者向上推力大于重力的两倍
	if (F_des(2) < 0.5 * param.mass * param.gra)
	{
		F_des = F_des / F_des(2) * (0.5 * param.mass * param.gra);
	}
	else if (F_des(2) > 2 * param.mass * param.gra)
	{
		F_des = F_des / F_des(2) * (2 * param.mass * param.gra);
	}

	// 角度限制幅度
	if (std::fabs(F_des(0)/F_des(2)) > std::tan(toRad(tilt_angle_max)))
	{
		F_des(0) = F_des(0)/std::fabs(F_des(0)) * F_des(2) * std::tan(toRad(tilt_angle_max));
	}

	// 角度限制幅度
	if (std::fabs(F_des(1)/F_des(2)) > std::tan(toRad(tilt_angle_max)))
	{
		F_des(1) = F_des(1)/std::fabs(F_des(1)) * F_des(2) * std::tan(toRad(tilt_angle_max));	
	}

	Vector3d z_b_des = F_des / F_des.norm();
	
	/////////////////////////////////////////////////
	// Z-X-Y Rotation Sequence                
	// Vector3d x_c_des = Vector3d(std::cos(yaw_des), sin(yaw_des), 0.0);
	// Vector3d y_b_des = z_b_des.cross(x_c_des) / z_b_des.cross(x_c_des).norm();
	// Vector3d x_b_des = y_b_des.cross(z_b_des);
	/////////////////////////////////////////////////

	/////////////////////////////////////////////////
	// Z-Y-X Rotation Sequence                
	Vector3d y_c_des = Vector3d(-std::sin(yaw_des), std::cos(yaw_des), 0.0);
	Vector3d x_b_des = y_c_des.cross(z_b_des) / y_c_des.cross(z_b_des).norm();
	Vector3d y_b_des = z_b_des.cross(x_b_des);
	///////////////////////////////////////////////// 

	Matrix3d R_des1; // it's wRb
	R_des1 << x_b_des, y_b_des, z_b_des;
	
	Matrix3d R_des2; // it's wRb
	R_des2 << -x_b_des, -y_b_des, z_b_des;
	
	Vector3d e1 = R_to_ypr(R_des1.transpose() * odom.q.toRotationMatrix());
	Vector3d e2 = R_to_ypr(R_des2.transpose() * odom.q.toRotationMatrix());

	Matrix3d R_des; // it's wRb

	if (e1.norm() < e2.norm())
	{
		R_des = R_des1;
	}
	else
	{
		R_des = R_des2;
	}


	// n3 api control in forward-left-up frame
	// F_des是位于ENU坐标系的,F_c是FLU
	Vector3d F_c = wRc.transpose() * F_des;
	// 无人机姿态的矩阵形式
	Matrix3d wRb_odom = odom.q.toRotationMatrix();
	// 第三列
	Vector3d z_b_curr = wRb_odom.col(2);
	// 机体系下的推力合力 相当于Rb * F_enu 惯性系到机体系
	double u1 = F_des.dot(z_b_curr);
	double fx = F_c(0);
	double fy = F_c(1);
	double fz = F_c(2);
	// 期望roll: 
	u.roll  = std::atan2(-fy, fz);
	u.pitch = std::atan2( fx, fz);
	// 油门 = 期望推力/最大推力
	// full_thrust = mass * gra / hov_percent;
	// 这里相当于认为油门是线性的,满足某种比例关系,即认为某个重量 = 悬停油门
	// 悬停油门与电机参数有关系,也取决于质量
	u.thrust = u1 / param.full_thrust;
	// VERT_THRU = 1.0;
	u.mode = Controller_Output_t::VERT_THRU;
	// use_yaw_rate_ctrl = false
	if(param.use_yaw_rate_ctrl){
		u.yaw_mode = Controller_Output_t::CTRL_YAW_RATE;
		u.yaw = u_yaw;
	}
	else{
		u.yaw_mode = Controller_Output_t::CTRL_YAW;
		u.yaw = des.yaw;
	}

	

};
