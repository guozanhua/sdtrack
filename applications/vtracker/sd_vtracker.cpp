// Copyright (c) George Washington University, all rights reserved.  See the
// accompanying LICENSE file for more information.
#undef NDEBUG
#include <assert.h>

// #define CHECK_NANS

#include <HAL/Camera/CameraDevice.h>
#include <HAL/Messages/Image.h>

#include <calibu/cam/camera_crtp.h>

#include "GetPot"
#include <sdtrack/TicToc.h>
#include <unistd.h>
#include <SceneGraph/SceneGraph.h>
#include <pangolin/pangolin.h>
#include <ba/BundleAdjuster.h>
#include "CVars/CVar.h"
#include <sdtrack/utils.h>
#include "math_types.h"
#include "gui_common.h"
#include "etc_common.h"
#include "vtrack-cvars.h"
#ifdef CHECK_NANS
#include <xmmintrin.h>
#endif

#include <sdtrack/semi_dense_tracker.h>

using namespace std;

uint32_t keyframe_tracks = UINT_MAX;
uint32_t frame_count = 0;
Sophus::SE3d last_t_ba, prev_delta_t_ba, prev_t_ba;

const int window_width = 640;
const int window_height = 480;
std::string g_usage = "SD VTRACKER. Example usage:\n"
    "-cam file:[loop=1]///Path/To/Dataset/[left,right]*pgm -cmod cameras.xml";
bool is_keyframe = true, is_prev_keyframe = true;
bool optimize_landmarks = true;
bool optimize_pose = true;
bool is_running = false;
bool is_stepping = false;
bool is_manual_mode = false;
bool do_start_new_landmarks = true;
int image_width;
int image_height;
calibu::Rig<Scalar> old_rig;
calibu::Rig<Scalar> rig;
hal::Camera camera_device;
sdtrack::SemiDenseTracker tracker;

TrackerGuiVars gui_vars;
std::shared_ptr<GetPot> cl;

// TrackCenterMap current_track_centers;
std::list<std::shared_ptr<sdtrack::DenseTrack>>* current_tracks = nullptr;
int last_optimization_level = 0;
// std::shared_ptr<sdtrack::DenseTrack> selected_track = nullptr;
std::shared_ptr<hal::Image> camera_img;
std::vector<std::shared_ptr<sdtrack::TrackerPose>> poses;
std::vector<std::unique_ptr<SceneGraph::GLAxis> > axes;
std::shared_ptr<SceneGraph::GLPrimitives<>> line_strip;
ba::BundleAdjuster<double, 1, 6, 0> bundle_adjuster;

// State variables
std::vector<cv::KeyPoint> keypoints;

Sophus::SE3d guess;


void DoBundleAdjustment(uint32_t num_active_poses, uint32_t id)
{
  if (reset_outliers) {
    for (std::shared_ptr<sdtrack::TrackerPose> pose : poses) {
      for (std::shared_ptr<sdtrack::DenseTrack> track: pose->tracks) {
        track->is_outlier = false;
      }
    }
    reset_outliers = false;
  }

  ba::Options<double> options;
  options.use_dogleg = use_dogleg;
  options.use_sparse_solver = true;
  options.param_change_threshold = 1e-10;
  options.error_change_threshold = 1e-3;
  options.use_robust_norm_for_proj_residuals = use_robust_norm_for_proj;
  options.projection_outlier_threshold = outlier_threshold;
  // options.error_change_threshold = 1e-3;
  // options.use_sparse_solver = false;
  uint32_t num_outliers = 0;
  Sophus::SE3d t_ba;
  uint32_t start_active_pose, start_pose;

  GetBaPoseRange(poses, num_active_poses, start_pose, start_active_pose);

  if (start_pose == poses.size()) {
    return;
  }

  bool all_poses_active = start_active_pose == start_pose;

  // Do a bundle adjustment on the current set
  if (current_tracks && poses.size() > 1) {
    std::shared_ptr<sdtrack::TrackerPose> last_pose = poses.back();
    bundle_adjuster.Init(options, poses.size(),
                         current_tracks->size() * poses.size());
    for (unsigned int cam_id = 0; cam_id < rig.cameras_.size(); ++cam_id) {
      bundle_adjuster.AddCamera(rig.cameras_[cam_id]);
    }

    // First add all the poses and landmarks to ba.
    for (uint32_t ii = start_pose ; ii < poses.size() ; ++ii) {
      std::shared_ptr<sdtrack::TrackerPose> pose = poses[ii];
      pose->opt_id[id] = bundle_adjuster.AddPose(
            pose->t_wp, ii >= start_active_pose );
      for (std::shared_ptr<sdtrack::DenseTrack> track: pose->tracks) {
        const bool constrains_active =
            track->keypoints.size() + ii > start_active_pose;
        if (track->num_good_tracked_frames <= 1 || track->is_outlier ||
            !constrains_active) {
          track->external_id[id] = UINT_MAX;
          continue;
        }
        Eigen::Vector4d ray;
        ray.head<3>() = track->ref_keypoint.ray;
        ray[3] = track->ref_keypoint.rho;
        ray = sdtrack::MultHomogeneous(
				       pose->t_wp  * rig.cameras_[track->ref_cam_id]->Pose(), ray);
        bool active = track->id != tracker.longest_track_id() ||
            !all_poses_active;
        if (!active) {
          std::cerr << "Landmark " << track->id << " inactive. " << std::endl;
        }
        track->external_id[id] =
            bundle_adjuster.AddLandmark(ray, pose->opt_id[id],
                                        track->ref_cam_id, active);
      }
    }

    // Now add all reprojections to ba)
    for (uint32_t ii = start_pose ; ii < poses.size() ; ++ii) {
      std::shared_ptr<sdtrack::TrackerPose> pose = poses[ii];
      for (std::shared_ptr<sdtrack::DenseTrack> track : pose->tracks) {
        if (track->external_id[id] == UINT_MAX) {
          continue;
        }
        for (uint32_t cam_id = 0; cam_id < rig.cameras_.size(); ++cam_id) {
          for (size_t jj = 0; jj < track->keypoints.size() ; ++jj) {
            if (track->keypoints[jj][cam_id].tracked) {
              const Eigen::Vector2d& z = track->keypoints[jj][cam_id].kp;
              bundle_adjuster.AddProjectionResidual(
                    z, pose->opt_id[id] + jj, track->external_id[id], cam_id);
            }
          }
        }
      }
    }

    // Optimize the poses
    bundle_adjuster.Solve(num_ba_iterations);

    // Get the pose of the last pose. This is used to calculate the relative
    // transform from the pose to the current pose.
    last_pose->t_wp = bundle_adjuster.GetPose(last_pose->opt_id[id]).t_wp;
    // std::cerr << "last pose t_wp: " << std::endl << last_pose->t_wp.matrix() <<
    //              std::endl;

    // Read out the pose and landmark values.
    for (uint32_t ii = start_pose ; ii < poses.size() ; ++ii) {
      std::shared_ptr<sdtrack::TrackerPose> pose = poses[ii];
      const ba::PoseT<double>& ba_pose =
          bundle_adjuster.GetPose(pose->opt_id[id]);

      pose->t_wp = ba_pose.t_wp;
      // Here the last pose is actually t_wb and the current pose t_wa.
      last_t_ba = t_ba;
      t_ba = last_pose->t_wp.inverse() * pose->t_wp;
      for (std::shared_ptr<sdtrack::DenseTrack> track: pose->tracks) {
        if (track->external_id[id] == UINT_MAX) {
          continue;
        }
        track->t_ba = t_ba;

        // Get the landmark location in the world frame.
        const Eigen::Vector4d& x_w =
            bundle_adjuster.GetLandmark(track->external_id[id]);
        double ratio =
            bundle_adjuster.LandmarkOutlierRatio(track->external_id[id]);
        auto landmark =
            bundle_adjuster.GetLandmarkObj(track->external_id[id]);

        if (do_outlier_rejection) {
          if (ratio > 0.3 &&
              ((track->keypoints.size() == num_ba_poses - 1) ||
               track->tracked == false)) {
            num_outliers++;
            track->is_outlier = true;
          } else {
            track->is_outlier = false;
          }
        }

        Eigen::Vector4d prev_ray;
        prev_ray.head<3>() = track->ref_keypoint.ray;
        prev_ray[3] = track->ref_keypoint.rho;
        // Make the ray relative to the pose.
        Eigen::Vector4d x_r =
            sdtrack::MultHomogeneous(
				     (pose->t_wp * rig.cameras_[track->ref_cam_id]->Pose()).inverse(), x_w);
        // Normalize the xyz component of the ray to compare to the original
        // ray.
        x_r /= x_r.head<3>().norm();
        track->ref_keypoint.rho = x_r[3];
      }
    }

  }
  std::cerr << "Rejected " << num_outliers << " outliers." << std::endl;
}

void UpdateCurrentPose()
{
  std::shared_ptr<sdtrack::TrackerPose> new_pose = poses.back();
  if (poses.size() > 1) {
    new_pose->t_wp = poses[poses.size() - 2]->t_wp * tracker.t_ba().inverse();
  }

  // Also use the current tracks to update the index of the earliest covisible
  // pose.
  size_t max_track_length = 0;
  for (std::shared_ptr<sdtrack::DenseTrack>& track : tracker.GetCurrentTracks()) {
    max_track_length = std::max(track->keypoints.size(), max_track_length);
  }
  new_pose->longest_track = max_track_length;
}

void BaAndStartNewLandmarks()
{
  if (!is_keyframe) {
    return;
  }

  //uint32_t keyframe_id = poses.size();

  double ba_time = sdtrack::Tic();
  if (do_bundle_adjustment) {
    DoBundleAdjustment(10, 0);
  }
  ba_time = sdtrack::Toc(ba_time);

  if (do_start_new_landmarks) {
    tracker.StartNewLandmarks();
  }

  std::shared_ptr<sdtrack::TrackerPose> new_pose = poses.back();
  // Update the tracks on this new pose.
  new_pose->tracks = tracker.GetNewTracks();

  if (!do_bundle_adjustment) {
    tracker.TransformTrackTabs(tracker.t_ba());
  }

  std::cerr << "Timings ba: " << ba_time << std::endl;
}

void ProcessImage(std::vector<cv::Mat>& images)
{
#ifdef CHECK_NANS
  _MM_SET_EXCEPTION_MASK(_MM_GET_EXCEPTION_MASK() &
                         ~(_MM_MASK_INVALID | _MM_MASK_OVERFLOW |
                           _MM_MASK_DIV_ZERO));
#endif

  frame_count++;
//  if (poses.size() > 100) {
//    exit(EXIT_SUCCESS);
//  }

  // If this is a keyframe, set it as one on the tracker.
  prev_delta_t_ba = tracker.t_ba() * prev_t_ba.inverse();

  if (is_prev_keyframe) {
    prev_t_ba = Sophus::SE3d();
  } else {
    prev_t_ba = tracker.t_ba();
  }

  // Add a pose to the poses array
  if (is_prev_keyframe) {
    std::shared_ptr<sdtrack::TrackerPose> new_pose(new sdtrack::TrackerPose);
    if (poses.size() > 0) {
      new_pose->t_wp = poses.back()->t_wp * last_t_ba.inverse();
    }
    poses.push_back(new_pose);
    axes.push_back(std::unique_ptr<SceneGraph::GLAxis>(
                      new SceneGraph::GLAxis(0.05)));
    gui_vars.scene_graph.AddChild(axes.back().get());
  }

  if (((double)tracker.num_successful_tracks() / (double)num_features) > 0.3) {
    guess = prev_delta_t_ba * prev_t_ba;
  } else {
    guess = Sophus::SE3d();
  }

  if(guess.translation() == Eigen::Vector3d(0,0,0) &&
     poses.size() > 1) {
    guess.translation() = Eigen::Vector3d(0,0,0.001);
  }

  std::cerr << "Guess: " << guess.matrix3x4() << std::endl;

  tracker.AddImage(images, guess);
  tracker.EvaluateTrackResiduals(0, tracker.GetImagePyramid(),
                                 tracker.GetCurrentTracks());

  if (!is_manual_mode) {
    sdtrack::OptimizationOptions options;
    options.optimize_landmarks = optimize_landmarks;
    options.optimize_pose = optimize_pose;
    tracker.OptimizeTracks(options);
    tracker.PruneTracks();
  }

  if (tracker.num_successful_tracks() < 10) {
    std::cerr << "Tracking failed. using guess." << std::endl;
    tracker.set_t_ba(guess);
    poses.back()->tracks.clear();
  }

  // Update the pose t_ab based on the result from the tracker.
  UpdateCurrentPose();

  if (do_keyframing) {
    const double track_ratio = (double)tracker.num_successful_tracks() /
        (double)keyframe_tracks;
    const double total_trans = tracker.t_ba().translation().norm();
    const double total_rot = tracker.t_ba().so3().log().norm();

    bool keyframe_condition = track_ratio < 0.8 || total_trans > 0.2 ||
        total_rot > 0.1;

    std::cerr << "\tRatio: " << track_ratio << " trans: " << total_trans <<
                 " rot: " << total_rot << std::endl;

    if (keyframe_tracks != 0) {
      if (keyframe_condition) {
        is_keyframe = true;
      } else {
        is_keyframe = false;
      }
    }

    // If this is a keyframe, set it as one on the tracker.
    prev_delta_t_ba = tracker.t_ba() * prev_t_ba.inverse();

    if (is_keyframe) {
      tracker.AddKeyframe();
    }
    is_prev_keyframe = is_keyframe;
  } else {
    tracker.AddKeyframe();
  }

  std::cerr << "Num successful : " << tracker.num_successful_tracks() <<
               " keyframe tracks: " << keyframe_tracks << std::endl;

  if (!is_manual_mode) {
    BaAndStartNewLandmarks();
  }

  if (is_keyframe) {
    std::cerr << "KEYFRAME." << std::endl;
    keyframe_tracks = tracker.GetCurrentTracks().size();
    std::cerr << "New keyframe tracks: " << keyframe_tracks << std::endl;
  } else {
    std::cerr << "NOT KEYFRAME." << std::endl;
  }

  current_tracks = &tracker.GetCurrentTracks();

#ifdef CHECK_NANS
  _MM_SET_EXCEPTION_MASK(_MM_GET_EXCEPTION_MASK() |
                         (_MM_MASK_INVALID | _MM_MASK_OVERFLOW |
                          _MM_MASK_DIV_ZERO));
#endif

  std::cerr << "FRAME : " << frame_count << " KEYFRAME: " << poses.size() <<
               std::endl;
}

void DrawImageData(uint32_t cam_id)
{
  if (cam_id == 0) {
    gui_vars.handler->track_centers.clear();
  }

  line_strip->Clear();
  for (uint32_t ii = 0; ii < poses.size() ; ++ii) {
    axes[ii]->SetPose(poses[ii]->t_wp.matrix());
    Eigen::Vector3f vertex = poses[ii]->t_wp.translation().cast<float>();
    line_strip->AddVertex(vertex);
  }

  // Draw the tracks
  for (std::shared_ptr<sdtrack::DenseTrack>& track : *current_tracks) {  
    Eigen::Vector2d center;
    if (track->keypoints.back()[cam_id].tracked || is_manual_mode) {
      DrawTrackData(track, image_width, image_height, center,
                    gui_vars.handler->selected_track == track, cam_id);
    }
    if (cam_id == 0) {
      gui_vars.handler->track_centers.push_back(
            std::pair<Eigen::Vector2d, std::shared_ptr<sdtrack::DenseTrack>>(
              center, track));
    }
  }

  // Populate the first column with the reference from the selected track.
  if (gui_vars.handler->selected_track != nullptr) {
    DrawTrackPatches(gui_vars.handler->selected_track, gui_vars.patches);
  }

  for (uint32_t cam_id = 0; cam_id < rig.cameras_.size(); ++cam_id) {
    gui_vars.camera_view[cam_id]->RenderChildren();
  }
}

bool LoadCameras()
{
  LoadCameraAndRig(*cl, camera_device, rig);
  //calibu::CreateFromOldRig(&old_rig, &rig);
  // rig.cameras_.resize(1);
  // rig.t_wc_.resize(1);
  return true;
}

void Run()
{
  std::vector<pangolin::GlTexture> gl_tex;

  // pangolin::Timer timer;
  bool capture_success = false;
  std::shared_ptr<hal::ImageArray> images = hal::ImageArray::Create();
  for (int ii = 0 ; ii < 10 ; ++ii) {
    camera_device.Capture(*images);
  }

  while(!pangolin::ShouldQuit()) {
    capture_success = false;
    const bool go = is_stepping;
    if (!is_running) {
      is_stepping = false;
    }
    // usleep(20000);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor4f(1.0f,1.0f,1.0f,1.0f);

    if (go) {
      capture_success = camera_device.Capture(*images);
    }

    if (capture_success) {
      gl_tex.resize(images->Size());

      for (uint32_t cam_id = 0 ; cam_id < (unsigned int) images->Size() ; ++cam_id) {
        if (!gl_tex[cam_id].tid) {
          camera_img = images->at(cam_id);
          GLint internal_format = (camera_img->Format() == GL_LUMINANCE ?
                                     GL_LUMINANCE : GL_RGBA);
          // Only initialise now we know format.
          gl_tex[cam_id].Reinitialise(
                camera_img->Width(), camera_img->Height(), internal_format,
                false, 0, camera_img->Format(), camera_img->Type(), 0);
        }
      }

      camera_img = images->at(0);
      image_width = camera_img->Width();
      image_height = camera_img->Height();
      gui_vars.handler->image_height = image_height;
      gui_vars.handler->image_width = image_width;

      std::vector<cv::Mat> cvmat_images;
      for (int ii = 0; ii < images->Size() ; ++ii) {
        cvmat_images.push_back(images->at(ii)->Mat());
      }
      ProcessImage(cvmat_images);
    }

    if (camera_img && camera_img->data()) {
      for (uint32_t cam_id = 0 ; cam_id < rig.cameras_.size() &&
	     cam_id < (unsigned int) images->Size(); ++cam_id) {
        camera_img = images->at(cam_id);
        gui_vars.camera_view[cam_id]->ActivateAndScissor();
        gl_tex[cam_id].Upload(camera_img->data(), camera_img->Format(),
                      camera_img->Type());
        gl_tex[cam_id].RenderToViewportFlipY();
        DrawImageData(cam_id);
      }

      gui_vars.grid_view->ActivateAndScissor(gui_vars.gl_render3d);

      if (draw_landmarks) {
        DrawLandmarks(min_lm_measurements_for_drawing, poses, rig,
                      gui_vars.handler, selected_track_id);
      }
    }
    pangolin::FinishFrame();
  }
}

void InitTracker()
{
  patch_size = 9;
  sdtrack::KeypointOptions keypoint_options;
  keypoint_options.gftt_feature_block_size = patch_size;
  keypoint_options.max_num_features = num_features * 2;
  keypoint_options.gftt_min_distance_between_features = 3;
  keypoint_options.gftt_absolute_strength_threshold = 0.005;
  sdtrack::TrackerOptions tracker_options;
  tracker_options.pyramid_levels = pyramid_levels;
  tracker_options.detector_type = sdtrack::TrackerOptions::Detector_GFTT;
  tracker_options.num_active_tracks = num_features;
  tracker_options.use_robust_norm_ = false;
  tracker_options.robust_norm_threshold_ = 30;
  tracker_options.patch_dim = patch_size;
  tracker_options.default_rho = 1.0/5.0;
  tracker_options.feature_cells = feature_cells;
  tracker_options.iteration_exponent = 2;
  tracker_options.center_weight = tracker_center_weight;
  tracker_options.dense_ncc_threshold = ncc_threshold;
  tracker_options.harris_score_threshold = 2e6;
  tracker_options.gn_scaling = 1.0;
  tracker.Initialize(keypoint_options, tracker_options, &rig);
}
void writePoses()
{
    // write all the poses to a file.
    std::ofstream pose_file("poses.txt", std::ios_base::trunc);
    for(vector<shared_ptr<sdtrack::TrackerPose>>::iterator pose = poses.begin(); pose != poses.end(); ++pose)
      {

      //Prepare the euler angle representation (rotation about x,y,z)
      //Order determines the output ordering
      //Want R = Ryaw*Rpitch*Rroll
	//or R = R(z) * R(y) * R(x)
       
	sdtrack::TrackerPose* thePose = (*pose).get();
	Sophus::SE3t t_wp = thePose->t_wp;
	auto rotMat = t_wp.rotationMatrix();

	cout << "Rotation Matrix:" << endl << rotMat << endl;
	
	//Invert the basis vectors to correspond to ROS standard (x fwd, y left, z up)

	Eigen::Vector3d ea = rotMat.eulerAngles(2, 1, 0);
	cout << "Euler angle representation:" << endl << ea << endl;

	//Compute it the manual way
	//Roll:
	ea[0] = atan2(rotMat(2,1), rotMat(2,2));
	//Pitch
	ea[1] = -asin(rotMat(2,0));
	//Yaw
	ea[2] = atan2(rotMat(1,0), rotMat(0,0));
	cout << "Euler angle representation: (manual)" << endl << ea << endl;

	pose_file << thePose->t_wp.translation().transpose().format(sdtrack::kLongCsvFmt);
      pose_file << "," << ea.transpose().format(sdtrack::kLongCsvFmt);
      pose_file << std::endl;
  
    }
    std::cerr << "Wrote poses" << std::endl;
}

void InitGui()
{
  InitTrackerGui(gui_vars, window_width, window_height, image_width,
                 image_height, rig.cameras_.size());
  line_strip.reset(new SceneGraph::GLPrimitives<>);
  gui_vars.scene_graph.AddChild(line_strip.get());

  pangolin::RegisterKeyPressCallback(
        pangolin::PANGO_SPECIAL + pangolin::PANGO_KEY_RIGHT,
        [&]() {
    is_stepping = true;
  });

  pangolin::RegisterKeyPressCallback(
        pangolin::PANGO_CTRL + 'r',
        [&]() {
    camera_img.reset();
    is_keyframe = true;
    is_prev_keyframe = true;
    is_running = false;
    InitTracker();
    poses.clear();
    gui_vars.scene_graph.Clear();
    gui_vars.scene_graph.AddChild(&gui_vars.grid);
    axes.clear();
    LoadCameras();
    prev_delta_t_ba = Sophus::SE3d();
    prev_t_ba = Sophus::SE3d();
    last_t_ba = Sophus::SE3d();
  });

  pangolin::RegisterKeyPressCallback(
        pangolin::PANGO_CTRL + 's',
        	  writePoses);

  pangolin::RegisterKeyPressCallback(' ', [&]() {
    is_running = !is_running;
  });

  pangolin::RegisterKeyPressCallback('b', [&]() {
    last_optimization_level = 0;
    tracker.OptimizeTracks(last_optimization_level,
                           optimize_landmarks, optimize_pose);
  });

  pangolin::RegisterKeyPressCallback('B', [&]() {
    do_bundle_adjustment = !do_bundle_adjustment;
    std::cerr << "Do BA:" << do_bundle_adjustment << std::endl;
  });

  pangolin::RegisterKeyPressCallback('S', [&]() {
    do_start_new_landmarks = !do_start_new_landmarks;
    std::cerr << "Do SNL:" << do_start_new_landmarks << std::endl;
  });

  pangolin::RegisterKeyPressCallback('2', [&]() {
    last_optimization_level = 2;
    tracker.OptimizeTracks(last_optimization_level,
                           optimize_landmarks, optimize_pose);
    UpdateCurrentPose();
  });

  pangolin::RegisterKeyPressCallback('3', [&]() {
    last_optimization_level = 3;
    tracker.OptimizeTracks(last_optimization_level,
                           optimize_landmarks, optimize_pose);
    UpdateCurrentPose();
  });

  pangolin::RegisterKeyPressCallback('1', [&]() {
    last_optimization_level = 1;
    tracker.OptimizeTracks(last_optimization_level,
                           optimize_landmarks, optimize_pose);
    UpdateCurrentPose();
  });

  pangolin::RegisterKeyPressCallback('0', [&]() {
    last_optimization_level = 0;
    tracker.OptimizeTracks(last_optimization_level,
                           optimize_landmarks, optimize_pose);
    UpdateCurrentPose();
  });

  pangolin::RegisterKeyPressCallback('9', [&]() {
    last_optimization_level = 0;
    tracker.OptimizeTracks(-1, optimize_landmarks, optimize_pose);
    UpdateCurrentPose();
  });

  pangolin::RegisterKeyPressCallback('p', [&]() {
    tracker.PruneTracks();
    // Update the pose t_ab based on the result from the tracker.
    UpdateCurrentPose();
    BaAndStartNewLandmarks();
  });

  pangolin::RegisterKeyPressCallback('l', [&]() {
    optimize_landmarks = !optimize_landmarks;
    std::cerr << "optimize landmarks: " << optimize_landmarks << std::endl;
  });

  pangolin::RegisterKeyPressCallback('c', [&]() {
    optimize_pose = !optimize_pose;
    std::cerr << "optimize pose: " << optimize_pose << std::endl;
  });

  pangolin::RegisterKeyPressCallback('m', [&]() {
    is_manual_mode = !is_manual_mode;
    std::cerr << "Manual mode:" << is_manual_mode << std::endl;
  });

  pangolin::RegisterKeyPressCallback('k', [&]() {
    sdtrack::AlignmentOptions options;
    options.apply_to_kp = true;
    tracker.Do2dAlignment(options,
                          tracker.GetImagePyramid(),
                          tracker.GetCurrentTracks(),
                          last_optimization_level);
  });
}

int main(int argc, char** argv) {
  srand(0);
  cl = std::shared_ptr<GetPot>(new GetPot(argc, argv));
  if (cl->search("--help")) {
    LOG(INFO) << g_usage;
    exit(-1);
  }

  if (cl->search("-startnow")) {
    is_running = true;
    is_stepping = true;
  }

  LOG(INFO) << "Initializing camera...";
  LoadCameras();

  InitTracker();

  InitGui();

  bundle_adjuster.debug_level_threshold = -1;

  Run();

  return 0;
}
