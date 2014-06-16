/**\file planar_localization.cpp
 * \brief Description...
 *
 * @version 1.0
 * @author Carlos Miguel Correia da Costa
 */

// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   <includes>   <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
#include "dynamic_robot_localization/planar/planar_localization.h"
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   </includes>  <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   <imports>   <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   </imports>  <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

namespace dynamic_robot_localization {

// =============================================================================  <public-section>  ============================================================================
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   <constructors-destructor>   <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
PlanarLocalization::PlanarLocalization(ros::NodeHandlePtr& node_handle, ros::NodeHandlePtr& private_node_handle) :
		map_received_(false), last_scan_time_(ros::Time::now()), number_poses_published_(0), node_handle_(node_handle), private_node_handle_(private_node_handle) {

	// subscription topic names fields
	private_node_handle_->param("pointcloud_topic", pointcloud_topic_, std::string("planar_pointcloud"));
	private_node_handle_->param("costmap_topic", costmap_topic_, std::string("map"));
	private_node_handle_->param("reference_cloud_topic", reference_cloud_topic_, std::string(""));

	// publish topic names
	private_node_handle_->param("reference_map_pointcloud_publish_topic", reference_map_pointcloud_publish_topic_, std::string("reference_map_pointcloud"));
	private_node_handle_->param("aligned_pointcloud_publish_topic", aligned_pointcloud_publish_topic_, std::string("aligned_pointcloud"));
	private_node_handle_->param("aligned_pointcloud_outliers_publish_topic", aligned_pointcloud_outliers_publish_topic_, std::string("aligned_pointcloud_outliers"));
	private_node_handle_->param("pose_publish_topic", pose_publish_topic_, std::string("initialpose"));

	// configuration fields
	private_node_handle_->param("publish_tf_map_odom", publish_tf_map_odom_, false);
	private_node_handle_->param("add_odometry_displacement", add_odometry_displacement_, false);
	private_node_handle_->param("reference_cloud_file_name", reference_cloud_file_name_, std::string(""));
	private_node_handle_->param("map_frame_id", map_frame_id_, std::string("map"));
	pose_to_tf_publisher_.setMapFrameId(map_frame_id_);
	private_node_handle_->param("base_link_frame_id", base_link_frame_id_, std::string("base_link"));
	pose_to_tf_publisher_.setBaseLinkFrameId(base_link_frame_id_);
	double max_seconds_scan_age;
	private_node_handle_->param("max_seconds_scan_age", max_seconds_scan_age, 0.5);
	max_seconds_scan_age_.fromSec(max_seconds_scan_age);
	double min_seconds_between_scan_registration;
	private_node_handle_->param("min_seconds_between_scan_registration", min_seconds_between_scan_registration, 0.05);
	min_seconds_between_scan_registration_.fromSec(min_seconds_between_scan_registration);
	double min_seconds_between_map_update;
	private_node_handle_->param("min_seconds_between_map_update", min_seconds_between_map_update, 5.0);
	min_seconds_between_map_update_.fromSec(min_seconds_between_map_update);


	// registration fields
	private_node_handle_->param("max_alignment_fitness", max_alignment_fitness_, 1e-2);
	private_node_handle_->param("max_transformation_angle", max_transformation_angle_, 1.59);
	private_node_handle_->param("max_transformation_distance", max_transformation_distance_, 2.5);
	private_node_handle_->param("max_inliers_distance", max_inliers_distance_, 0.01);

	double max_correspondence_distance;
	double transformation_epsilon;
	double euclidean_fitness_epsilon;
	int max_number_of_registration_iterations;
	int max_number_of_ransac_iterations;
	double ransac_outlier_rejection_threshold;

	private_node_handle_->param("max_correspondence_distance", max_correspondence_distance, 2.5);
	private_node_handle_->param("transformation_epsilon", transformation_epsilon, 1e-8);
	private_node_handle_->param("euclidean_fitness_epsilon", euclidean_fitness_epsilon, 1e-6);
	private_node_handle_->param("max_number_of_registration_iterations", max_number_of_registration_iterations, 500);
	private_node_handle_->param("max_number_of_ransac_iterations", max_number_of_ransac_iterations, 500);
	private_node_handle_->param("ransac_outlier_rejection_threshold", ransac_outlier_rejection_threshold, 0.05);
	planar_matcher_ = PlanarMatcher(max_correspondence_distance, transformation_epsilon, euclidean_fitness_epsilon, max_number_of_registration_iterations);
	planar_matcher_.getCloudMatcher()->setRANSACIterations(max_number_of_ransac_iterations);
	planar_matcher_.getCloudMatcher()->setRANSACOutlierRejectionThreshold(ransac_outlier_rejection_threshold);

	if (publish_tf_map_odom_) {
		pose_to_tf_publisher_.readConfigurationFromParameterServer(node_handle, private_node_handle);
		pose_to_tf_publisher_.publishInitialPoseFromParameterServer();
	}

	dynamic_reconfigure::Server<dynamic_robot_localization::PlanarLocalizationConfig>::CallbackType callback_dynamic_reconfigure =
				boost::bind(&dynamic_robot_localization::PlanarLocalization::dynamicReconfigureCallback, this, _1, _2);
		dynamic_reconfigure_server_.setCallback(callback_dynamic_reconfigure);

}

PlanarLocalization::~PlanarLocalization() {
}
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   </constructors-destructor>  <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   <PlanarLocalization-functions>   <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
void dynamic_robot_localization::PlanarLocalization::startLocalization() {
	laserscan_cloud_subscriber_ = node_handle_->subscribe(pointcloud_topic_, 100, &dynamic_robot_localization::PlanarLocalization::processLaserScanCloud, this);

	map_pointcloud_publisher_ = node_handle_->advertise<sensor_msgs::PointCloud2>(reference_map_pointcloud_publish_topic_, 10);
	aligned_pointcloud_publisher_ = node_handle_->advertise<sensor_msgs::PointCloud2>(aligned_pointcloud_publish_topic_, 10);
	aligned_pointcloud_outliers_publisher_ = node_handle_->advertise<sensor_msgs::PointCloud2>(aligned_pointcloud_outliers_publish_topic_, 10);
	pose_publisher_ = node_handle_->advertise<geometry_msgs::PoseWithCovarianceStamped>(pose_publish_topic_, 10);

	if (reference_cloud_file_name_.empty()) {
		if (!reference_cloud_topic_.empty()) {
			reference_cloud_subscriber_ = node_handle_->subscribe(reference_cloud_topic_, 5, &dynamic_robot_localization::PlanarLocalization::processReferenceCloud, this);
		} else {
			if (!costmap_topic_.empty())
				costmap_subscriber_ = node_handle_->subscribe(costmap_topic_, 5, &dynamic_robot_localization::PlanarLocalization::processReferenceOccupancyGrid, this);
		}
	} else {
		if (planar_matcher_.loadReferencePointCloud(reference_cloud_file_name_)) {
			map_received_ = true;
			publishReferenceCloud();
		}
	}

	if (publish_tf_map_odom_) {
		ros::Rate publish_rate(pose_to_tf_publisher_.getPublishRate());
		while (ros::ok()) {
			pose_to_tf_publisher_.publishTFMapToOdom();
			publish_rate.sleep();
			ros::spinOnce();
		}
	} else {
		ros::spin();
	}
}


void PlanarLocalization::stopLocalization() {
	laserscan_cloud_subscriber_.shutdown();
	costmap_subscriber_.shutdown();
	reference_cloud_subscriber_.shutdown();
	map_pointcloud_publisher_.shutdown();
	aligned_pointcloud_publisher_.shutdown();
	aligned_pointcloud_outliers_publisher_.shutdown();
	pose_publisher_.shutdown();
}


void PlanarLocalization::processReferenceOccupancyGrid(const nav_msgs::OccupancyGridConstPtr& planar_map) {
	if (!map_received_ || (ros::Time::now() - last_map_received_time_) > min_seconds_between_map_update_) {
		last_map_received_time_ = ros::Time::now();
		if (planar_matcher_.createReferencePointcloudFromMap(planar_map)) {
			map_received_ = true;
			publishReferenceCloud();
		}
	}
}


void PlanarLocalization::processReferenceCloud(const sensor_msgs::PointCloud2ConstPtr& reference_cloud) {
	PlanarMatcher::PointCloudTarget::Ptr pointcloud(new PlanarMatcher::PointCloudTarget());
	pcl::fromROSMsg(*reference_cloud, *pointcloud); // without normals
	planar_matcher_.setReferencePointcloud(pointcloud);
	/*pcl::fromROSMsg(*laserscan_cloud, *pointcloud_xyz); // with normals
	planar_matcher_.computeNormals(pointcloud_xyz, pointcloud); // with normals*/
	map_received_ = true;
	publishReferenceCloud();
}

void PlanarLocalization::processLaserScanCloud(const sensor_msgs::PointCloud2ConstPtr& laserscan_cloud) {
	ros::Duration scan_age = ros::Time::now() - laserscan_cloud->header.stamp;
	ros::Duration elapsed_time_since_last_scan = ros::Time::now() - last_scan_time_;

	ROS_DEBUG_STREAM("Received point cloud with " << (laserscan_cloud->width * laserscan_cloud->height) << " points at time stamp " << laserscan_cloud->header.stamp);

	if (map_received_ && elapsed_time_since_last_scan > min_seconds_between_scan_registration_ && scan_age < max_seconds_scan_age_) {
		last_scan_time_ = ros::Time::now();

		tf2::Transform pose_tf;
		if (!pose_to_tf_publisher_.getTfCollector().lookForTransform(pose_tf, laserscan_cloud->header.frame_id, base_link_frame_id_, laserscan_cloud->header.stamp)) {
			ROS_DEBUG_STREAM("Dropping scan because tf between " << laserscan_cloud->header.frame_id << " and " << base_link_frame_id_ << " isn't available");
			return;
		}

//		PointCloudXYZ::Ptr pointcloud_xyz(new PointCloudXYZ()); // with normals
		PlanarMatcher::PointCloudSource::Ptr pointcloud(new PlanarMatcher::PointCloudSource());
		PlanarMatcher::PointCloudSource::Ptr aligned_pointcloud(new PlanarMatcher::PointCloudSource());
		pcl::fromROSMsg(*laserscan_cloud, *pointcloud); // without normals
		/*pcl::fromROSMsg(*laserscan_cloud, *pointcloud_xyz); // with normals
		planar_matcher_.computeNormals(pointcloud_xyz, pointcloud); // with normals*/
		if (reference_cloud_file_name_.empty() && reference_cloud_topic_.empty()) {
			resetPointCloudHeight(pointcloud);
		}


		ROS_DEBUG_STREAM("Aligning point cloud... (current time " << ros::Time::now() << ")");
		double alignmentFitness = planar_matcher_.alignPlanarPointclouds(pointcloud, aligned_pointcloud); // alignmentFitness < 0 if there was no alignment
		ROS_DEBUG_STREAM("Registered cloud with " << alignmentFitness << " alignment fitness, with scan with time " << laserscan_cloud->header.stamp << " (current time " << ros::Time::now() << ")");

		if (alignmentFitness >= 0 && alignmentFitness < max_alignment_fitness_) {
			tf2::Transform pose_correction;
			laserscan_to_pointcloud::tf_rosmsg_eigen_conversions::transformMatrixToTF2(planar_matcher_.getCloudMatcher()->getFinalTransformation(), pose_correction);

			double transform_distance = pose_correction.getOrigin().length();
			double transform_angle = pose_correction.getRotation().getAngle();
			if (transform_distance > max_transformation_distance_ || transform_angle > max_transformation_angle_) {
				ROS_DEBUG_STREAM("Dropping scan because pose correction exceeded bounds (translation: " << transform_distance << " | rotation: " << transform_angle);
				return;
			}

			tf2::Transform pose_corrected = pose_correction * pose_tf;

			geometry_msgs::PoseWithCovarianceStampedPtr pose(new geometry_msgs::PoseWithCovarianceStamped());
			pose->header.frame_id = laserscan_cloud->header.frame_id;
			pose->header.seq = number_poses_published_++;

			if (add_odometry_displacement_) {
				pose->header.stamp = ros::Time::now();
			} else {
				pose->header.stamp = laserscan_cloud->header.stamp;
			}

			if (add_odometry_displacement_)
				pose_to_tf_publisher_.addOdometryDisplacementToTransform(pose_corrected, laserscan_cloud->header.stamp, pose->header.stamp);

			if (publish_tf_map_odom_)
				pose_to_tf_publisher_.publishTFMapToOdom(pose_corrected);

			laserscan_to_pointcloud::tf_rosmsg_eigen_conversions::transformTF2ToMsg(pose_corrected, pose->pose.pose);
//			todo: fill covariance
//			pose->pose.covariance

			pose_publisher_.publish(pose);

			if (!aligned_pointcloud_publish_topic_.empty() && aligned_pointcloud_publisher_.getNumSubscribers() > 0) {
				sensor_msgs::PointCloud2Ptr aligned_pointcloud_msg(new sensor_msgs::PointCloud2());
				pcl::toROSMsg(*aligned_pointcloud, *aligned_pointcloud_msg);
				aligned_pointcloud_publisher_.publish(aligned_pointcloud_msg);
			}

			publishOutliers(laserscan_cloud, aligned_pointcloud);
		} else {
			ROS_DEBUG_STREAM("Failed registration with fitness " << alignmentFitness);
		}
	} else {
		if (!map_received_) {
			ROS_DEBUG_STREAM("Discarded cloud because there is no reference cloud to compare to!");
		} else {
			ROS_DEBUG_STREAM("Discarded cloud with age " << scan_age.toSec());
		}
	}
}


void PlanarLocalization::resetPointCloudHeight(PlanarMatcher::PointCloudSource::Ptr& pointcloud, float height) {
	size_t number_of_points = pointcloud->points.size();
	for (size_t i = 0; i < number_of_points; ++i) {
		pointcloud->points[i].z = height;
	}
}


void PlanarLocalization::publishOutliers(const sensor_msgs::PointCloud2ConstPtr& laserscan_cloud, PlanarMatcher::PointCloudSource::Ptr& pointcloud) {
	if (!aligned_pointcloud_outliers_publish_topic_.empty() && aligned_pointcloud_outliers_publisher_.getNumSubscribers() > 0) {
		std::vector<int> nn_indices(1);
		std::vector<float> nn_distances(1);

		// init pointcloud:
		sensor_msgs::PointCloud2Ptr outliers_cloud = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2());
		size_t number_points_in_cloud = 0;
		size_t number_reserved_points = pointcloud->points.size();
		float* pointcloud_data_position = NULL;

		outliers_cloud->header.seq = laserscan_cloud->header.seq;
		outliers_cloud->header.stamp = laserscan_cloud->header.stamp;
		outliers_cloud->header.frame_id = laserscan_cloud->header.frame_id;
		outliers_cloud->height = 1;
		outliers_cloud->width = 0;
		outliers_cloud->fields.clear();
		outliers_cloud->fields.resize(3);
		outliers_cloud->fields[0].name = "x";
		outliers_cloud->fields[0].offset = 0;
		outliers_cloud->fields[0].datatype = sensor_msgs::PointField::FLOAT32;
		outliers_cloud->fields[0].count = 1;
		outliers_cloud->fields[1].name = "y";
		outliers_cloud->fields[1].offset = 4;
		outliers_cloud->fields[1].datatype = sensor_msgs::PointField::FLOAT32;
		outliers_cloud->fields[1].count = 1;
		outliers_cloud->fields[2].name = "z";
		outliers_cloud->fields[2].offset = 8;
		outliers_cloud->fields[2].datatype = sensor_msgs::PointField::FLOAT32;
		outliers_cloud->fields[2].count = 1;
		outliers_cloud->point_step = 12;

		outliers_cloud->data.resize(number_reserved_points * outliers_cloud->point_step); // resize to fit all points and avoid memory moves
		pointcloud_data_position = (float*) (&outliers_cloud->data[0]);

		// find outliers
		for (size_t i = 0; i < pointcloud->points.size(); ++i) {
			PlanarMatcher::PointSource& point = pointcloud->points[i];
			planar_matcher_.getCloudMatcher()->getSearchMethodTarget()->nearestKSearch(point, 1, nn_indices, nn_distances);
			if (nn_distances[0] > max_inliers_distance_) {
				*pointcloud_data_position++ = point.x;
				*pointcloud_data_position++ = point.y;
				*pointcloud_data_position++ = point.z;
				++number_points_in_cloud;
			}
		}

		// publish cloud
		outliers_cloud->width = number_points_in_cloud;
		outliers_cloud->row_step = outliers_cloud->width * outliers_cloud->point_step;
		outliers_cloud->data.resize(outliers_cloud->height * outliers_cloud->row_step); // resize to shrink the vector size to the real number of points inserted

		if (number_points_in_cloud > 1) {
			aligned_pointcloud_outliers_publisher_.publish(outliers_cloud);
		}
	}
}


void PlanarLocalization::publishReferenceCloud() {
	if (!reference_map_pointcloud_publish_topic_.empty() /*&& map_pointcloud_publisher_.getNumSubscribers() > 0*/) {
		sensor_msgs::PointCloud2Ptr reference_pointcloud(new sensor_msgs::PointCloud2());
		pcl::toROSMsg(*(planar_matcher_.getReferencePointcloud()), *reference_pointcloud);
		reference_pointcloud->header.frame_id = map_frame_id_;
		reference_pointcloud->header.stamp = ros::Time::now();
		map_pointcloud_publisher_.publish(reference_pointcloud);
	}
}


void PlanarLocalization::dynamicReconfigureCallback(dynamic_robot_localization::PlanarLocalizationConfig& config, uint32_t level) {
	if (level == 1) {
			ROS_INFO_STREAM("LaserScanToPointcloudAssembler dynamic reconfigure (level=" << level << ") -> " \
					<< "\n\t[planar_pointcloud_topic]: " 					<< pointcloud_topic_ 														<< " -> " << config.pointcloud_topic \
					<< "\n\t[costmap_topic]: " 								<< costmap_topic_															<< " -> " << config.costmap_topic \
					<< "\n\t[reference_map_pointcloud_publish_topic]: "		<< reference_map_pointcloud_publish_topic_ 									<< " -> " << config.reference_map_pointcloud_publish_topic \
					<< "\n\t[aligned_pointcloud_publish_topic]: "			<< aligned_pointcloud_publish_topic_ 										<< " -> " << config.aligned_pointcloud_publish_topic \
					<< "\n\t[pose_publish_topic]: " 						<< pose_publish_topic_ 														<< " -> " << config.pose_publish_topic \
					<< "\n\t[publish_tf_map_odom]: "					 	<< publish_tf_map_odom_				 										<< " -> " << config.publish_tf_map_odom \
					<< "\n\t[add_odometry_displacement]: "					<< add_odometry_displacement_				 								<< " -> " << config.add_odometry_displacement \
					<< "\n\t[base_link_frame_id]: "					 		<< base_link_frame_id_				 										<< " -> " << config.base_link_frame_id \
					<< "\n\t[max_seconds_scan_age]: " 						<< max_seconds_scan_age_.toSec()											<< " -> " << config.max_seconds_scan_age \
					<< "\n\t[min_seconds_between_laserscan_registration]: " << min_seconds_between_scan_registration_.toSec()							<< " -> " << config.min_seconds_between_laserscan_registration \
					<< "\n\t[min_seconds_between_map_update]: " 			<< min_seconds_between_map_update_.toSec() 									<< " -> " << config.min_seconds_between_map_update \
					<< "\n\t[max_alignment_fitness]: " 						<< max_alignment_fitness_					 								<< " -> " << config.max_alignment_fitness \
					<< "\n\t[max_transformation_angle]: " 					<< max_transformation_angle_				 								<< " -> " << config.max_transformation_angle \
					<< "\n\t[max_transformation_distance]: " 				<< max_transformation_distance_					 							<< " -> " << config.max_transformation_distance \
					<< "\n\t[max_correspondence_distance]: " 				<< planar_matcher_.getCloudMatcher()->getMaxCorrespondenceDistance() 		<< " -> " << config.max_correspondence_distance \
					<< "\n\t[transformation_epsilon]: " 					<< planar_matcher_.getCloudMatcher()->getTransformationEpsilon() 			<< " -> " << config.transformation_epsilon \
					<< "\n\t[euclidean_fitness_epsilon]: " 					<< planar_matcher_.getCloudMatcher()->getEuclideanFitnessEpsilon() 			<< " -> " << config.euclidean_fitness_epsilon \
					<< "\n\t[max_number_of_registration_iterations]: " 		<< planar_matcher_.getCloudMatcher()->getMaximumIterations()				<< " -> " << config.max_number_of_registration_iterations \
					<< "\n\t[max_number_of_ransac_iterations]: " 			<< planar_matcher_.getCloudMatcher()->getRANSACIterations()					<< " -> " << config.max_number_of_ransac_iterations \
					<< "\n\t[ransac_outlier_rejection_threshold]: " 		<< planar_matcher_.getCloudMatcher()->getRANSACOutlierRejectionThreshold()	<< " -> " << config.ransac_outlier_rejection_threshold);

			// Subscribe topics
			if (!config.pointcloud_topic.empty() && pointcloud_topic_ != config.pointcloud_topic) {
				pointcloud_topic_ = config.pointcloud_topic;
				laserscan_cloud_subscriber_.shutdown();
				laserscan_cloud_subscriber_ = node_handle_->subscribe(pointcloud_topic_, 100, &dynamic_robot_localization::PlanarLocalization::processLaserScanCloud, this);
			}

			if (!config.costmap_topic.empty() && costmap_topic_ != config.costmap_topic) {
				costmap_topic_ = config.costmap_topic;
				costmap_subscriber_.shutdown();
				costmap_subscriber_ = node_handle_->subscribe(costmap_topic_, 5, &dynamic_robot_localization::PlanarLocalization::processReferenceOccupancyGrid, this);
			}

			// Publish topics
			if (!config.reference_map_pointcloud_publish_topic.empty() && reference_map_pointcloud_publish_topic_ != config.reference_map_pointcloud_publish_topic) {
				reference_map_pointcloud_publish_topic_ = config.reference_map_pointcloud_publish_topic;
				map_pointcloud_publisher_.shutdown();
				map_pointcloud_publisher_ = node_handle_->advertise<sensor_msgs::PointCloud2>(reference_map_pointcloud_publish_topic_, 10);
			}

			if (!config.aligned_pointcloud_publish_topic.empty() && aligned_pointcloud_publish_topic_ != config.aligned_pointcloud_publish_topic) {
				aligned_pointcloud_publish_topic_ = config.aligned_pointcloud_publish_topic;
				aligned_pointcloud_publisher_.shutdown();
				aligned_pointcloud_publisher_ = node_handle_->advertise<sensor_msgs::PointCloud2>(aligned_pointcloud_publish_topic_, 10);
			}

			if (!config.pose_publish_topic.empty() && pose_publish_topic_ != config.pose_publish_topic) {
				pose_publish_topic_ = config.pose_publish_topic;
				pose_publisher_.shutdown();
				pose_publisher_ = node_handle_->advertise<geometry_msgs::PoseWithCovarianceStamped>(pose_publish_topic_, 10);
			}

			// Configurations
			publish_tf_map_odom_ = config.publish_tf_map_odom;
			add_odometry_displacement_ = config.add_odometry_displacement;
			base_link_frame_id_ = config.base_link_frame_id;
			pose_to_tf_publisher_.setBaseLinkFrameId(base_link_frame_id_);
			max_seconds_scan_age_.fromSec(config.max_seconds_scan_age);
			min_seconds_between_scan_registration_.fromSec(config.min_seconds_between_laserscan_registration);
			min_seconds_between_map_update_.fromSec(config.min_seconds_between_map_update);

			// ICP configuration
			max_alignment_fitness_ = config.max_alignment_fitness;
			max_transformation_angle_ = config.max_transformation_angle;
			max_transformation_distance_ = config.max_transformation_distance;
			planar_matcher_.getCloudMatcher()->setMaxCorrespondenceDistance(config.max_correspondence_distance);
			planar_matcher_.getCloudMatcher()->setTransformationEpsilon(config.transformation_epsilon);
			planar_matcher_.getCloudMatcher()->setEuclideanFitnessEpsilon(config.euclidean_fitness_epsilon);
			planar_matcher_.getCloudMatcher()->setMaximumIterations(config.max_number_of_registration_iterations);
			planar_matcher_.getCloudMatcher()->setRANSACIterations(config.max_number_of_ransac_iterations);
			planar_matcher_.getCloudMatcher()->setRANSACOutlierRejectionThreshold(config.ransac_outlier_rejection_threshold);
		}
}
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   </PlanarLocalization-functions>  <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
// =============================================================================  </public-section>  ===========================================================================

// =============================================================================   <protected-section>   =======================================================================
// =============================================================================   </protected-section>  =======================================================================

// =============================================================================   <private-section>   =========================================================================
// =============================================================================   </private-section>  =========================================================================

} /* namespace dynamic_robot_localization */

