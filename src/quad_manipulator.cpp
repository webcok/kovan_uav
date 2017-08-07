#include <ros/ros.h>
#include <std_srvs/Empty.h>
#include <control_toolbox/pid.h>
#include <gazebo_msgs/ModelStates.h>
#include <tf/transform_listener.h>

#include <geometry_msgs/Twist.h>
#include <geometry_msgs/PoseStamped.h>

#include <hector_uav_msgs/PoseActionGoal.h>
#include <hector_uav_msgs/EnableMotors.h>
#include <hector_uav_msgs/Done.h>

#include <boost/bind.hpp>
#include <stdlib.h>
#include <fstream>
#include <std_msgs/String.h>
#include <map>
#include <cmath>

#define EQUAL_CONST 0.5


void goalPoseCallback(const geometry_msgs::PoseStamped::ConstPtr& msg);
void quadPoseCallback(const geometry_msgs::PoseStamped::ConstPtr& msg);


//std::vector<std::thread*> quadrotorThreads;
struct 
{
	control_toolbox::Pid x, y, z, yaw;
} pid_;

unsigned int total_model_count = 0;

// THERE IS AMBIGUITY ABOUT HOW TO PUT THIS INTO A THREAD AND START ITS SERVING.
bool isEqual(double a, double b) { return abs(a - b) > EQUAL_CONST ? false : true; }
class Wrapper{
public:
	class QuadController {
		public:
			ros::NodeHandle node;
			ros::ServiceClient engage_motors;
			ros::Subscriber goal_sub, quad_sub;
			ros::Publisher quad_vel, quad_done;
			geometry_msgs::PoseStamped goalPose, quadPose;

			std::string quad_name;
			double yaw_command;
			bool desired_updated;
			bool desired_achived;
			bool operating;

			QuadController(ros::NodeHandle _node,std::string uav_name) 
			{
				
				activeQuadrotors.insert(std::pair<std::string, Wrapper::QuadController*>(uav_name,this));

				quad_name = uav_name;
				node = _node;
				desired_updated = false;
				desired_achived = false;
				operating = false;
				yaw_command = 0;
				engage_motors = node.serviceClient<hector_uav_msgs::EnableMotors>(/*"/"+quad_name+*/"/enable_motors");
				hector_uav_msgs::EnableMotors srv;
				srv.request.enable = true;
				if(engage_motors.call(srv)){
					if(srv.response.success){
						ROS_INFO("Motors are enabled");		
					}
				}
				goal_sub = node.subscribe<geometry_msgs::PoseStamped>(/*"/" + quad_name + */"/move_base_simple/goal", 10, 
					boost::bind(&QuadController::goalPoseCallback, this,quad_name, _1));
				quad_sub = node.subscribe<geometry_msgs::PoseStamped>(/*"/"+quad_name+*/"/ground_truth_to_tf/pose", 10, 
					boost::bind(&QuadController::quadPoseCallback, this,quad_name, _1));
				quad_vel = node.advertise<geometry_msgs::Twist>(/*"/"+quad_name+*/"/cmd_vel", 10);
				quad_done = node.advertise<hector_uav_msgs::Done>(/*"/"+quad_name+*/"Done", 10);
			}

			void goalPoseCallback(const std::string& robot_frame,const geometry_msgs::PoseStamped::ConstPtr& msg) 
			{
				QuadController* correspondingQuad = activeQuadrotors[robot_frame];
				operating = true;
				double x = msg->pose.position.x;
				double y = msg->pose.position.y;
				double z = msg->pose.position.z;

				double temp;
				tf::Quaternion q(msg->pose.orientation.x, msg->pose.orientation.y, 
					msg->pose.orientation.z, msg->pose.orientation.w);
				tf::Matrix3x3(q).getRPY(temp, temp, correspondingQuad->yaw_command);

				if((correspondingQuad->goalPose.pose.position.x == x) 
					&& (correspondingQuad->goalPose.pose.position.y == y) 
					&& (correspondingQuad->goalPose.pose.position.z == z))
					return;
				correspondingQuad->goalPose.pose.position.x = x;
				correspondingQuad->goalPose.pose.position.y = y;
				correspondingQuad->goalPose.pose.position.z = z;
				correspondingQuad->desired_updated = true;
				correspondingQuad->desired_achived = false;

				/*ROS_INFO("Goal is set for [%s]: %f, %f, %f",robot_frame.c_str(),correspondingQuad->goalPose.pose.position.x, 
					correspondingQuad->goalPose.pose.position.y, correspondingQuad->goalPose.pose.position.z);*/
			}
			
			void quadPoseCallback(const std::string& robot_frame,const geometry_msgs::PoseStamped::ConstPtr& msg) 
			{
				//ROS_INFO("robot_frame = %s",robot_frame.c_str());
				Wrapper::QuadController* correspondingQuad = Wrapper::activeQuadrotors[robot_frame];

				double goalX = correspondingQuad->goalPose.pose.position.x;
				double goalY = correspondingQuad->goalPose.pose.position.y;
				double goalZ = correspondingQuad->goalPose.pose.position.z;
				correspondingQuad->quadPose.pose.position.x = msg->pose.position.x;
				correspondingQuad->quadPose.pose.position.y = msg->pose.position.y;
				correspondingQuad->quadPose.pose.position.z = msg->pose.position.z;

				geometry_msgs::Twist vel_msg;
				ros::Duration period(0.1);

				if(correspondingQuad->shouldWait() == false)
				{
					double x = pid_.x.computeCommand(goalX - correspondingQuad->quadPose.pose.position.x, period);
					double y = pid_.y.computeCommand(goalY - correspondingQuad->quadPose.pose.position.y, period);
					double z = pid_.z.computeCommand(goalZ - correspondingQuad->quadPose.pose.position.z, period);

					tf::Quaternion q(msg->pose.orientation.x, msg->pose.orientation.y,
						msg->pose.orientation.z, msg->pose.orientation.w);
					double yaw = tf::getYaw(q);
					double yaw_error = correspondingQuad->yaw_command - yaw;
				    // detect wrap around pi and compensate

					if (yaw_error > 3.14) 
						yaw_error -= 2 * 3.14;
					else if (yaw_error < -3.14) 
						yaw_error += 2 * 3.14;

					// if UAV is in the correct position it will return to CommandDone
					if( operating && isEqual(goalX, correspondingQuad->quadPose.pose.position.x) 
						&& isEqual(goalY, correspondingQuad->quadPose.pose.position.y) 
						&& isEqual(goalZ, correspondingQuad->quadPose.pose.position.z) )
					{
						hector_uav_msgs::Done done_send;

						done_send.commandDone = true;
						done_send.position.x = goalX;
						done_send.position.y = goalY;
						done_send.position.z = goalZ;
						done_send.orientation.x = 0;
						done_send.orientation.y = 0;
						done_send.orientation.z = 0;

						correspondingQuad->quad_done.publish(done_send);
					}
					// corrects the position of the quadro by giving velocity
		
					vel_msg.linear.x = x * 0.3;
					vel_msg.linear.y = y * 0.3;
					vel_msg.linear.z = z;
					vel_msg.angular.z = 3 * pid_.yaw.computeCommand(yaw_error, period);

					correspondingQuad->quad_vel.publish(vel_msg);
				}
			}

			~QuadController() {}

			bool shouldWait() { return (!desired_updated) && (desired_achived); }
	};

	static std::map<std::string, QuadController*> activeQuadrotors;
};

//Recall that, static member variables of a class must be properly initialized/constructed outside of the class.
std::map<std::string, Wrapper::QuadController*> Wrapper::activeQuadrotors;

void spawnCallback(ros::NodeHandle& nh,const gazebo_msgs::ModelStatesConstPtr& msg)
{
	size_t name_size = msg->name.size();
	if(name_size>1 && name_size!=total_model_count)
	{
		for(size_t i=1;i<name_size;i++)
		{
			size_t found = msg->name[i].find("uav");
			std::map <std::string, Wrapper::QuadController*>::iterator it;
			it = Wrapper::activeQuadrotors.find(msg->name[i]);

			if (found!=std::string::npos && it == Wrapper::activeQuadrotors.end())
			{
				Wrapper::QuadController* new_quad_controller = new Wrapper::QuadController(nh,msg->name[i]);
				Wrapper::activeQuadrotors.insert(std::pair<std::string, Wrapper::QuadController*>(msg->name[i],new_quad_controller));
				//ROS_INFO("[%s] is registered to active Quadrotors",msg->name[i].c_str());
			}
			//ROS_INFO("Name is = %s", msg->name[i].c_str());
		}
	}
		
}
int main(int argc, char **argv) 
{
	ros::init(argc, argv, "quad_manipulator");
	ros::NodeHandle root_node;
	pid_.x.init(ros::NodeHandle(root_node, "x"));
	pid_.y.init(ros::NodeHandle(root_node, "y"));
	pid_.z.init(ros::NodeHandle(root_node, "z"));
	pid_.yaw.init(ros::NodeHandle(root_node, "yaw"));

	/*ros::Subscriber model_listener_sub = root_node.subscribe<gazebo_msgs::ModelStates>("/gazebo/model_states", 2, 
		boost::bind(spawnCallback, root_node, _1));*/

	Wrapper::QuadController* new_quad_controller1 = new Wrapper::QuadController(root_node, "quadrotor");

    //Wrapper::QuadController* new_quad_controller1 = new Wrapper::QuadController(root_node,"uav1");
    /*Wrapper::QuadController* new_quad_controller2 = new Wrapper::QuadController(root_node,"uav2");
    Wrapper::QuadController* new_quad_controller3 = new Wrapper::QuadController(root_node,"uav3");
    Wrapper::QuadController* new_quad_controller4 = new Wrapper::QuadController(root_node,"uav4");*/

	ros::Rate rate(200);
	while(ros::ok()) {
		ros::spinOnce();
	}
	
	//ros::spin();
	return 0;
}