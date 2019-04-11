/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/perception/camera/app/cipv_camera.h"

#include <cmath>
#include <limits>

#include "cyber/common/log.h"
#include "modules/common/math/line_segment2d.h"

namespace apollo {
namespace perception {

Cipv::Cipv(void) {}

Cipv::~Cipv(void) {}

bool Cipv::Init(const Eigen::Matrix3d &homography_im2car) {
  b_image_based_cipv_ = false;
  debug_level_ = 0;  // 0: no debug message
                     // 1: minimal output
                     // 2: some important output
                     // 3: verbose message
                     // 4: visualization
                     // 5: all
                     // -x: specific debugging, where x is the specific number
  time_unit_ = AVERAGE_FRATE_RATE;
  homography_im2car_ = homography_im2car;
  homography_car2im_ = homography_im2car.inverse();
  return true;
}

// Distance from a point to a line segment
bool Cipv::DistanceFromPointToLineSegment(const Point2Df &point,
                                          const Point2Df &line_seg_start_point,
                                          const Point2Df &line_seg_end_point,
                                          float *distance) {
  common::math::Vec2d p = {point[0], point[1]};
  common::math::LineSegment2d line_seg(
      {line_seg_start_point[0], line_seg_start_point[1]},
      {line_seg_end_point[0], line_seg_end_point[1]});
  if (line_seg.length_sqr() <= B_FLT_EPSILON) {
    // line length = 0
    return false;
  }
  *distance = static_cast<float>(line_seg.DistanceTo(p));
  return true;
}

// Select CIPV among multiple objects
bool Cipv::GetEgoLane(const std::vector<base::LaneLine> &lane_objects,
                      EgoLane *egolane_image,
                      EgoLane *egolane_ground, bool *b_left_valid,
                      bool *b_right_valid) {
  for (size_t i = 0; i < lane_objects.size(); ++i) {
    if (lane_objects[i].pos_type == base::LaneLinePositionType::EGO_LEFT) {
      if (debug_level_ >= 2) {
        AINFO << "[GetEgoLane]LEFT_"
              << "lane_objects[i].curve_image_point_set.size(): "
              << lane_objects[i].curve_image_point_set.size();
      }
      if (lane_objects[i].curve_image_point_set.size() <
          MIN_LANE_LINE_LENGTH_FOR_CIPV_DETERMINATION) {
        *b_left_valid = false;
      } else {
        *b_left_valid = true;

        for (size_t j = 0;
             j < lane_objects[i].curve_image_point_set.size();
            ++j) {
          Eigen::Vector2f image_point(
            lane_objects[i].curve_image_point_set[j].x,
            lane_objects[i].curve_image_point_set[j].y);
          egolane_image->left_line.line_point.push_back(image_point);

          Eigen::Vector2f ground_point(
            lane_objects[i].curve_car_coord_point_set[j].x,
            lane_objects[i].curve_car_coord_point_set[j].y);
          egolane_ground->left_line.line_point.push_back(ground_point);
          if (debug_level_ >= 2) {
            AINFO << "Left ego lane[" << j << "]: " << ground_point[0] << ", "
                  << ground_point[1];
          }
        }
      }
    } else if (
        lane_objects[i].pos_type == base::LaneLinePositionType::EGO_RIGHT) {
      if (debug_level_ >= 2) {
        AINFO << "[GetEgoLane]RIGHT_"
              << "lane_objects[i].curve_image_point_set.size(): "
              << lane_objects[i].curve_image_point_set.size();
      }
      if (lane_objects[i].curve_image_point_set.size() <
          MIN_LANE_LINE_LENGTH_FOR_CIPV_DETERMINATION) {
        *b_right_valid = false;
      } else {
        *b_right_valid = true;
        for (size_t j = 0;
             j < lane_objects[i].curve_image_point_set.size();
             ++j) {
          Eigen::Vector2f image_point(
            lane_objects[i].curve_image_point_set[j].x,
            lane_objects[i].curve_image_point_set[j].y);
          egolane_image->right_line.line_point.push_back(image_point);

          Eigen::Vector2f ground_point(
            lane_objects[i].curve_car_coord_point_set[j].x,
            lane_objects[i].curve_car_coord_point_set[j].y);
          egolane_ground->right_line.line_point.push_back(ground_point);
          if (debug_level_ >= 2) {
            AINFO << "Right ego lane[" << j << "]: " << ground_point[0] << ", "
                  << ground_point[1];
          }
        }
      }
    }
  }
  return true;
}

// Make a virtual lane line using a reference lane line and its offset distance
bool Cipv::MakeVirtualLane(const LaneLineSimple &ref_lane_line,
                           const float yaw_rate,
                           const float offset_distance,
                           LaneLineSimple *virtual_lane_line) {
  // TODO(techoe): Use union of lane line and yaw_rate path to define the
  // virtual lane
  virtual_lane_line->line_point.clear();
  if (b_image_based_cipv_ == false) {
    for (uint32_t i = 0; i < ref_lane_line.line_point.size(); ++i) {
      Eigen::Vector2f virtual_line_point(
          ref_lane_line.line_point[i][0],
          ref_lane_line.line_point[i][1] + offset_distance);
      virtual_lane_line->line_point.push_back(virtual_line_point);
    }
  } else {
    // Image based extension requires to reproject virtual laneline points to
    // image space.
  }
  return true;
}

float Cipv::VehicleDynamics(const uint32_t tick, const float yaw_rate,
                            const float velocity, const float time_unit,
                            float *x, float *y) {
  // straight model;
  *x = time_unit * velocity;
  *y = 0.0f;

  // float theta = time_unit_ * yaw_rate;
  // float displacement = time_unit_ * velocity;

  // Eigen::Rotation2Df rot2d(theta);
  // Eigen::Vector2f trans;

  // trans(0) = displacement * cos(theta);
  // trans(1) = displacement * sin(theta);

  // motion_2d.block(0, 0, 2, 2) = rot2d.toRotationMatrix().transpose();
  // motion_2d.block(0, 2, 2, 1) = -rot2d.toRotationMatrix().transpose() *
  // trans;
  return true;
}

// Make a virtual lane line using a yaw_rate
bool Cipv::MakeVirtualEgoLaneFromYawRate(const float yaw_rate,
                                         const float velocity,
                                         const float offset_distance,
                                         LaneLineSimple *left_lane_line,
                                         LaneLineSimple *right_lane_line) {
  // TODO(techoe): Use union of lane line and yaw_rate path to decide the
  // virtual lane
  float x = 0.0f;
  float y = 0.0f;
  left_lane_line->line_point.clear();
  right_lane_line->line_point.clear();

  if (b_image_based_cipv_ == false) {
    for (uint32_t i = 0; i < 120; i += 5) {
      VehicleDynamics(i, yaw_rate, velocity, time_unit_, &x, &y);
      Eigen::Vector2f left_point(x, y + offset_distance);
      left_lane_line->line_point.push_back(left_point);
      Eigen::Vector2f right_point(x, y - offset_distance);
      right_lane_line->line_point.push_back(right_point);
    }
  } else {
    // Image based extension requires to reproject virtual laneline points to
    // image space.
  }
  return true;
}

// Elongate lane line
bool Cipv::ElongateEgoLane(const std::vector<base::LaneLine> &lane_objects,
                           const bool b_left_valid, const bool b_right_valid,
                           const float yaw_rate, const float velocity,
                           EgoLane *egolane_image, EgoLane *egolane_ground) {
  float offset_distance = EGO_CAR_HALF_VIRTUAL_LANE;
  // When left lane line is available
  if (b_left_valid && b_right_valid) {
    // elongate both lanes or do nothing
    if (debug_level_ >= 2) {
      AINFO << "Both lanes are fine";
    }
    // When only left lane line is available
  } else if (!b_left_valid && b_right_valid) {
    // Generate virtual left lane based on right lane
    offset_distance =
      -static_cast<float>(fabs(egolane_ground->right_line.line_point[0][1]) +
      EGO_CAR_HALF_VIRTUAL_LANE);
    MakeVirtualLane(egolane_ground->right_line, yaw_rate, offset_distance,
                    &egolane_ground->left_line);
    if (debug_level_ >= 2) {
      AINFO << "Made left lane with offset: " << offset_distance;
    }

    // When only right lane line is available
  } else if (b_left_valid && !b_right_valid) {
    // Generate virtual right lane based on left lane
    offset_distance =
      static_cast<float>(fabs(egolane_ground->left_line.line_point[0][1]) +
      EGO_CAR_HALF_VIRTUAL_LANE);
    MakeVirtualLane(egolane_ground->left_line, yaw_rate, offset_distance,
                    &egolane_ground->right_line);
    if (debug_level_ >= 2) {
      AINFO << "Made right lane with offset: " << offset_distance;
    }

    // When there is no lane lines available
  } else {  // if (!b_left_valid && !b_right_valid)
    // Generate new egolane using yaw-rate and yaw-angle
    MakeVirtualEgoLaneFromYawRate(yaw_rate, velocity, offset_distance,
                                  &egolane_ground->left_line,
                                  &egolane_ground->right_line);
    if (debug_level_ >= 2) {
      AINFO << "Made both lane_objects";
    }
  }

  return true;
}

// Get closest edge of an object in image coordinate
bool Cipv::FindClosestEdgeOfObjectImage(
  const std::shared_ptr<base::Object> &object,
  const EgoLane &egolane_image,
  LineSegment2Df *closted_object_edge) {
  float size_x = object->size(0);
  float size_y = object->size(1);
  float size_z = object->size(2);
  if (size_x < 1.0e-2 && size_y < 1.0e-2 && size_z < 1.0e-2) {
    // size_x = 0.1;
    // size_y = 0.1;
    // size_z = 0.1;
    return false;
  }
  // float center_x = static_cast<float>(object->center(0));
  // float center_y = static_cast<float>(object->center(1));
  float center_x =
    static_cast<float>(object->camera_supplement.local_center[0]);
  float center_y =
    static_cast<float>(object->camera_supplement.local_center[1]);
  // Direction should be changed based on alpha angle and more.
  float direction_x = object->direction(0);
  float direction_y = object->direction(1);
  float x1 = size_x / 2;
  float x2 = 0 - x1;
  float y1 = size_y / 2;
  float y2 = 0 - y1;
  float len = static_cast<float>(
              sqrt(direction_x * direction_x + direction_y * direction_y));
  float cos_theta = direction_x / len;
  float sin_theta = -direction_y / len;

  // Check if object direction is less than +-45 degree
  // angle = atan2(y, x)
  if (fabs(atan2(object->direction(1), object->direction(0))) <=
      FOURTY_FIVE_DEGREE) {
    // get back of the vehicle
    closted_object_edge->start_point[0] =
        x2 * cos_theta + y1 * sin_theta + center_x;
    closted_object_edge->start_point[1] =
        y1 * cos_theta - x2 * sin_theta + center_y;

    closted_object_edge->end_point[0] =
        x2 * cos_theta + y2 * sin_theta + center_x;
    closted_object_edge->end_point[1] =
        y2 * cos_theta - x2 * sin_theta + center_y;

    // If a vehicle faces side way, extract the side edge of a vehicle
  } else if (atan2(object->direction[1], object->direction[0]) >
             FOURTY_FIVE_DEGREE) {
    // get left side of the vehicle
    closted_object_edge->start_point[0] =
        x2 * cos_theta + y1 * sin_theta + center_x;
    closted_object_edge->start_point[1] =
        y1 * cos_theta - x2 * sin_theta + center_y;

    closted_object_edge->end_point[0] =
        x2 * cos_theta + y1 * sin_theta + center_x;
    closted_object_edge->end_point[1] =
        y1 * cos_theta - x2 * sin_theta + center_y;
  } else if (atan2(object->direction[1], object->direction[0]) <
             -FOURTY_FIVE_DEGREE) {
    // get right side of the vehicle

    closted_object_edge->start_point[0] =
        x1 * cos_theta + y2 * sin_theta + center_x;
    closted_object_edge->start_point[1] =
        y2 * cos_theta - x1 * sin_theta + center_y;

    closted_object_edge->end_point[0] =
        x2 * cos_theta + y2 * sin_theta + center_x;
    closted_object_edge->end_point[1] =
        y2 * cos_theta - x2 * sin_theta + center_y;
  } else {
    // don't get front of vehicle
    closted_object_edge->start_point[0] =
        x1 * cos_theta + y1 * sin_theta + center_x;
    closted_object_edge->start_point[1] =
        y1 * cos_theta - x1 * sin_theta + center_y;

    closted_object_edge->end_point[0] =
        x1 * cos_theta + y2 * sin_theta + center_x;
    closted_object_edge->end_point[1] =
        y2 * cos_theta - x1 * sin_theta + center_y;
  }

  return true;
}
// Get closest edge of an object in ground coordinate
// TODO(techoe): This function should be changed to find min-y and max-y edges
// to decide CIPV.
bool Cipv::FindClosestEdgeOfObjectGround(
  const std::shared_ptr<base::Object> &object,
  const EgoLane &egolane_ground,
  LineSegment2Df *closted_object_edge) {
  if (debug_level_ >= 2) {
    AINFO << "object->track_id: " << object->track_id;
    // AINFO << "object->length: " << object->length;
    // AINFO << "object->width: " << object->width;
    // AINFO << "object->height: " << object->height;
  }
  float size_x = object->size(0);
  float size_y = object->size(1);
  float size_z = object->size(2);
  if (size_x < 1.0e-2 && size_y < 1.0e-2 && size_z < 1.0e-2) {
    // size_x = 0.1;
    // size_y = 0.1;
    // size_z = 0.1;
    return false;
  }

  base::RectF rect(object->camera_supplement.box);
  float footprint_x = static_cast<float>(rect.x + rect.width * 0.5);
  float footprint_y = static_cast<float>(rect.y + rect.height);
  float center_x;
  float center_y;
  image2ground(footprint_x, footprint_y, &center_x, &center_y);

  Eigen::Vector3d pos;
  pos << object->camera_supplement.local_center[0],
      object->camera_supplement.local_center[1],
      object->camera_supplement.local_center[2];
  double theta_ray = atan2(pos[0], pos[2]);
  double theta = object->camera_supplement.alpha + theta_ray;
  if (theta > M_PI_2) {
    theta = theta - M_PI_2;
  }

  if (debug_level_ >= 3) {
    AINFO << "object->center[0]: " << object->center[0];
    AINFO << "object->center[1]: " << object->center[1];
    AINFO << "object->center[2]: " << object->center[2];
    AINFO << "object->anchor_point[0]: " << object->anchor_point[0];
    AINFO << "object->anchor_point[1]: " << object->anchor_point[1];
    AINFO << "object->anchor_point[2]: " << object->anchor_point[2];
    AINFO << "object->direction[0]: " << object->direction[0];
    AINFO << "object->direction[1]: " << object->direction[1];
    AINFO << "object->direction[2]: " << object->direction[2];
    AINFO << "center_x: " << center_x;
    AINFO << "center_y: " << center_y;
  }
  // float center_x = static_cast<float>(object->center(0));
  // float center_y = static_cast<float>(object->center(1));
  // float direction_x = object->direction(0);
  // float direction_y = object->direction(1);
  float x1 = size_x * 0.5f;
  float x2 = -x1;
  float y1 = size_y * 0.5f;
  float y2 = -y1;
  // float len = static_cast<float>(
  //             sqrt(direction_x * direction_x + direction_y * direction_y));
  // if (len < B_FLT_EPSILON) {
  //   return false;
  // }
  float cos_theta = static_cast<float>(cos(theta));  // direction_x / len;
  float sin_theta = static_cast<float>(sin(theta));  // -direction_y / len;

  Point2Df p[4];

  p[0][0] = x2 * cos_theta + y1 * sin_theta + center_x;
  p[0][1] = y1 * cos_theta - x2 * sin_theta + center_y;

  p[1][0] = x2 * cos_theta + y2 * sin_theta + center_x;
  p[1][1] = y2 * cos_theta - x2 * sin_theta + center_y;

  p[2][0] = x1 * cos_theta + y1 * sin_theta + center_x;
  p[2][1] = y1 * cos_theta - x1 * sin_theta + center_y;

  p[3][0] = x1 * cos_theta + y2 * sin_theta + center_x;
  p[3][1] = y2 * cos_theta - x1 * sin_theta + center_y;

  if (debug_level_ >= 2) {
    AINFO << "P0(" << p[0][0] << ", " << p[0][1] << ")";
    AINFO << "P1(" << p[1][0] << ", " << p[1][1] << ")";
    AINFO << "P2(" << p[2][0] << ", " << p[2][1] << ")";
    AINFO << "P3(" << p[3][0] << ", " << p[3][1] << ")";
  }

  float closest_x = MAX_FLOAT;
  float second_closest_x = MAX_FLOAT - 1.0f;
  int32_t closest_index = -1;
  int32_t second_closest_index = -1;
  for (int32_t i = 0; i < 4; i++) {
    if (p[i][0] <= closest_x) {
      second_closest_index = closest_index;
      second_closest_x = closest_x;
      closest_index = i;
      closest_x = p[i][0];
    } else if (p[i][0] <= second_closest_x) {
      second_closest_index = i;
      second_closest_x = p[i][0];
    }
  }
  if (p[closest_index][1] >= p[second_closest_index][1]) {
    closted_object_edge->start_point[0] = p[closest_index][0];
    closted_object_edge->start_point[1] = p[closest_index][1];

    closted_object_edge->end_point[0] = p[second_closest_index][0];
    closted_object_edge->end_point[1] = p[second_closest_index][1];
  } else {
    closted_object_edge->start_point[0] = p[second_closest_index][0];
    closted_object_edge->start_point[1] = p[second_closest_index][1];

    closted_object_edge->end_point[0] = p[closest_index][0];
    closted_object_edge->end_point[1] = p[closest_index][1];
  }

  // Added filter to consider an object only in front of ego-car.
  if (p[closest_index][0] < 0) {
    return false;
  }

  if (debug_level_ >= 2) {
    AINFO << "start(" << closted_object_edge->start_point[0] << ", "
          << closted_object_edge->start_point[1] << ")->";
    AINFO << "end(" << closted_object_edge->end_point[0] << ", "
          << closted_object_edge->end_point[1] << ")";
  }
  return true;
}

// Check if the distance between lane and object are OK
bool Cipv::AreDistancesSane(const float distance_start_point_to_right_lane,
                            const float distance_start_point_to_left_lane,
                            const float distance_end_point_to_right_lane,
                            const float distance_end_point_to_left_lane) {
  float distance = -1.0f;
  if (distance_start_point_to_right_lane > MAX_DIST_OBJECT_TO_LANE_METER) {
    if (debug_level_ >= 1) {
      AINFO << "distance from start to right lane(" << distance
            << " m) is too long";
    }
    return false;
  }
  if (distance_start_point_to_left_lane > MAX_DIST_OBJECT_TO_LANE_METER) {
    if (debug_level_ >= 1) {
      AINFO << "distance from start to left lane(" << distance
            << " m) is too long";
    }
    return false;
  }
  if (distance_end_point_to_right_lane > MAX_DIST_OBJECT_TO_LANE_METER) {
    if (debug_level_ >= 1) {
      AINFO << "distance from end to right lane(" << distance
            << " m) is too long";
    }
    return false;
  }
  if (distance_end_point_to_left_lane > MAX_DIST_OBJECT_TO_LANE_METER) {
    if (debug_level_ >= 1) {
      AINFO << "distance from end to left lane(" << distance
            << " m) is too long";
    }
    return false;
  }
  distance = static_cast<float>(fabs(distance_start_point_to_right_lane -
                  distance_end_point_to_right_lane));
  if (distance > MAX_VEHICLE_WIDTH_METER) {
    if (debug_level_ >= 1) {
      AINFO << "width of vehicle (" << distance << " m) is too long";
    }
    return false;
  }

  distance = static_cast<float>(
    fabs(distance_end_point_to_left_lane - distance_start_point_to_left_lane));
  if (distance > MAX_VEHICLE_WIDTH_METER) {
    if (debug_level_ >= 1) {
      AINFO << "width of vehicle (" << distance << " m) is too long";
    }
    return false;
  }
  // put more conditions here if required.

  // AINFO << "Distances are sane!";

  return true;
}

// Check if a point is left of a line segment
bool Cipv::IsPointLeftOfLine(const Point2Df &point,
                             const Point2Df &line_seg_start_point,
                             const Point2Df &line_seg_end_point) {
  float cross_product = ((line_seg_end_point[0] - line_seg_start_point[0]) *
                         (point[1] - line_seg_start_point[1])) -
                        ((line_seg_end_point[1] - line_seg_start_point[1]) *
                         (point[0] - line_seg_start_point[0]));

  if (cross_product > 0.0f) {
    if (debug_level_ >= 2) {
      AINFO << "point (" << point[0] << ", " << point[1]
            << ") is left of line_segment (" << line_seg_start_point[0] << ", "
            << line_seg_start_point[1] << ")->(" << line_seg_end_point[0]
            << ", " << line_seg_end_point[1]
            << "), cross_product: " << cross_product;
    }
    return true;
  } else {
    if (debug_level_ >= 2) {
      AINFO << "point (" << point[0] << ", " << point[1]
            << ") is right of line_segment (" << line_seg_start_point[0] << ", "
            << line_seg_start_point[1] << ")->(" << line_seg_end_point[0]
            << ", " << line_seg_end_point[1]
            << "), cross_product: " << cross_product;
    }
    return false;
  }
}
// Check if the object is in the lane in image space
bool Cipv::IsObjectInTheLaneImage(const std::shared_ptr<base::Object> &object,
                                  const EgoLane &egolane_image) {
  return true;
}

// Check if the object is in the lane in ego-ground space
//  |           |
//  | *------*  |
//  |         *-+-----*
//  |           |  *--------* <- closest edge of object
// *+------*    |
//  |           |
// l_lane     r_lane
bool Cipv::IsObjectInTheLaneGround(const std::shared_ptr<base::Object> &object,
                                   const EgoLane &egolane_ground) {
  LineSegment2Df closted_object_edge;
  bool b_left_lane_clear = false;
  bool b_right_lane_clear = false;
  float shortest_distance = 0.0f;
  float distance = 0.0f;
  float shortest_distance_start_point_to_right_lane = 0.0f;
  float shortest_distance_start_point_to_left_lane = 0.0f;
  float shortest_distance_end_point_to_right_lane = 0.0f;
  float shortest_distance_end_point_to_left_lane = 0.0f;

  int closest_index = -1;
  // Find closest edge of a given object bounding box
  float b_valid_object = FindClosestEdgeOfObjectGround(object, egolane_ground,
                                                       &closted_object_edge);
  if (!b_valid_object) {
    if (debug_level_ >= 1) {
      ADEBUG << "The closest edge of an object is not available";
    }
    return false;
  }

  if (debug_level_ >= 3) {
    AINFO << "egolane_ground.left_line.line_point.size(): "
          << egolane_ground.left_line.line_point.size();
  }
  if (egolane_ground.left_line.line_point.size() <= 1) {
    if (debug_level_ >= 1) {
      AINFO << "No left lane";
    }
    return false;
  }

  // Check end_point and left lane
  closest_index = -1;
  shortest_distance = MAX_FLOAT;
  for (size_t i = 0; i + 1 < egolane_ground.left_line.line_point.size(); ++i) {
    // If a end point is in the closest left lane line segments
    distance = MAX_FLOAT;
    if (DistanceFromPointToLineSegment(
            closted_object_edge.end_point,
            egolane_ground.left_line.line_point[i],
            egolane_ground.left_line.line_point[i + 1], &distance) == true) {
      if (distance < shortest_distance) {
        closest_index = static_cast<int>(i);
        shortest_distance = distance;
      }
    }
  }
  // When the closest line segment was found
  if (closest_index >= 0) {
    // Check if the end point is on the right of the line segment
    if (debug_level_ >= 3) {
      AINFO << "[Left] closest_index: " << closest_index
            << ", shortest_distance: " << shortest_distance;
    }
    if (IsPointLeftOfLine(
            closted_object_edge.end_point,
            egolane_ground.left_line.line_point[closest_index],
            egolane_ground.left_line.line_point[closest_index + 1]) == false) {
      b_left_lane_clear = true;
    }
  }

  if (debug_level_ >= 3) {
    AINFO << "egolane_ground.right_line.line_point.size(): "
          << egolane_ground.right_line.line_point.size();
  }
  // Check start_point and right lane
  if (egolane_ground.right_line.line_point.size() <= 1) {
    if (debug_level_ >= 1) {
      AINFO << "No right lane";
    }
    return false;
  }
  closest_index = -1;
  shortest_distance = MAX_FLOAT;
  for (size_t i = 0; i + 1 < egolane_ground.right_line.line_point.size(); ++i) {
    // If a end point is in the closest right lane line segments
    distance = MAX_FLOAT;
    if (DistanceFromPointToLineSegment(
            closted_object_edge.start_point,
            egolane_ground.right_line.line_point[i],
            egolane_ground.right_line.line_point[i + 1], &distance) == true) {
      if (distance < shortest_distance) {
        closest_index = static_cast<int>(i);
        shortest_distance = distance;
      }
    }
  }
  // When the closest line segment was found
  if (closest_index >= 0) {
    if (debug_level_ >= 3) {
      AINFO << "[right] closest_index: " << closest_index
            << ", shortest_distance: " << shortest_distance;
    }
    // Check if the end point is on the right of the line segment
    if (IsPointLeftOfLine(
            closted_object_edge.start_point,
            egolane_ground.right_line.line_point[closest_index],
            egolane_ground.right_line.line_point[closest_index + 1]) == true) {
      b_right_lane_clear = true;
    }
  }

  // Check if the distance is sane
  if (AreDistancesSane(shortest_distance_start_point_to_right_lane,
                       shortest_distance_start_point_to_left_lane,
                       shortest_distance_end_point_to_right_lane,
                       shortest_distance_end_point_to_left_lane)) {
    return (b_left_lane_clear && b_right_lane_clear);

  } else {
    return false;
  }

  return true;
}

// Check if the object is in the lane in ego-ground space
bool Cipv::IsObjectInTheLane(const std::shared_ptr<base::Object> &object,
                             const EgoLane &egolane_image,
                             const EgoLane &egolane_ground) {
  if (b_image_based_cipv_ == true) {
    return IsObjectInTheLaneImage(object, egolane_image);
  } else {
    return IsObjectInTheLaneGround(object, egolane_ground);
  }
}

// =====================================================================
// Decide CIPV among multiple objects
bool Cipv::DetermineCipv(const std::vector<base::LaneLine> &lane_objects,
                         const CipvOptions &options,
                         std::vector<std::shared_ptr<base::Object>> *objects) {
  if (debug_level_ >= 3) {
    AINFO << "Cipv Got SensorObjects with size of " << objects->size();
    AINFO << "Cipv Got lane object with size of"
          << lane_objects.size();
  }

  float yaw_rate = options.yaw_rate;
  float velocity = options.velocity;
  int32_t cipv_index = -1;
  //    int32_t old_cipv_track_id = sensor_objects.cipv_track_id;
  int32_t cipv_track_id = -1;
  // AINFO<<"static_cast<int32_t>(objects.size(): "
  //           << static_cast<int32_t>(objects.size());
  bool b_left_valid = false;
  bool b_right_valid = false;
  static int32_t old_cipv_index = -2;  // need to be changed
  EgoLane egolane_image;
  EgoLane egolane_ground;

  // Get ego lanes (in both image and ground coordinate)
  GetEgoLane(lane_objects, &egolane_image, &egolane_ground,
             &b_left_valid, &b_right_valid);
  ElongateEgoLane(lane_objects, b_left_valid, b_right_valid,
                  yaw_rate, velocity, &egolane_image, &egolane_ground);

  for (int32_t i = 0; i < static_cast<int32_t>(objects->size());
        ++i) {
     if (debug_level_ >= 2) {
      AINFO << "objects[" << i <<"]->track_id: " << (*objects)[i]->track_id;
     }
    if (IsObjectInTheLane((*objects)[i], egolane_image,
                           egolane_ground) == true) {
       if (cipv_index < 0 ||
          (*objects)[i]->center[0] <
              (*objects)[cipv_index]->center[0]) {
         // cipv_index is not set or if objects[i] is closer than
         // objects[cipv_index] in ego-x coordinate
        // AINFO << "objects[i]->center[0]: "
        //            << objects[i]->center[0];
        // AINFO << "objects[cipv_index]->center[0]: "
        //            << objects[cipv_index]->center[0];
        cipv_index = i;
        cipv_track_id = (*objects)[i]->track_id;
       }

      if (debug_level_ >= 2) {
        AINFO << "current cipv_index: " << cipv_index;
      }
    }
  }
  if (debug_level_ >= 1) {
    AINFO << "old_cipv_index: " << old_cipv_index;
  }
  if (cipv_index >= 0) {
//  if (old_cipv_index != cipv_index && cipv_index >= 0) {
    // AINFO << "(*objects)[cipv_index]->b_cipv: "
    //             << (*objects)[cipv_index]->b_cipv;
    // AINFO << "sensor_objects.cipv_index: "
    //            << sensor_objects.cipv_index;
    // AINFO << "sensor_objects.cipv_track_id: "
    //            << sensor_objects.cipv_track_id;
    if (old_cipv_index >= 0 && old_cipv_index != cipv_index) {
      // AINFO << "(*objects)[old_cipv_index]->b_cipv: "
      //             << (*objects)[old_cipv_index]->b_cipv;
      (*objects)[old_cipv_index]->b_cipv = false;
    }
    (*objects)[cipv_index]->b_cipv = true;
    // sensor_objects.cipv_index = cipv_index;
    // sensor_objects.cipv_track_id = cipv_track_id;
    if (debug_level_ >= 1) {
      AINFO << "final cipv_index: " << cipv_index;
      AINFO << "final cipv_track_id: " << cipv_track_id;
      // AINFO << "CIPV Index is changed from " << old_cipv_index << "th
      // object to "
      //            << cipv_index << "th object.";
      // AINFO << "CIPV Track_ID is changed from " << old_cipv_track_id <<
      // " to "
      //            << cipv_track_id << ".";
    }
  } else {
    if (debug_level_ >= 1) {
      AINFO << "No cipv";
    }
  }
  old_cipv_index = cipv_index;

  return true;
}


bool Cipv::TranformPoint(const Eigen::VectorXf& in,
                         const Eigen::Matrix4f& motion_matrix,
                         Eigen::Vector3d* out) {
  CHECK(in.rows() == motion_matrix.cols());
  Eigen::VectorXf trans_pt = motion_matrix * in;
  if (fabs(trans_pt[3]) < EPSILON) {
    return false;
  } else {
    trans_pt /= trans_pt[3];
  }
  *out << trans_pt[0], trans_pt[1], trans_pt[2];
  return true;
}

bool Cipv::CollectDrops(const base::MotionBufferPtr &motion_buffer,
                        std::vector<std::shared_ptr<base::Object>>* objects) {
  int motion_size = static_cast<int>(motion_buffer->size());
  if (debug_level_ >= 2) {
    AINFO << " motion_size: " << motion_size;
  }
  if (motion_size <= 0) {
    ADEBUG << " motion_size: " << motion_size;
    return false;
  }
  // std::map<int, std::vector<std::pair<float, float>>>
  //     tmp_object_trackjectories;
  // std::swap(object_trackjectories_, tmp_object_trackjectories);

  if (debug_level_ >= 2) {
    AINFO << "object_trackjectories_.size(): " << object_trackjectories_.size();
  }
  for (auto obj : *objects) {
    int cur_id = obj->track_id;
    if (debug_level_ >= 2) {
      AINFO << "target ID: " << cur_id;
    }
    // for (auto point : tmp_object_trackjectories[cur_id]) {
    //   object_trackjectories_[cur_id].push_back(point);
    // }

    // If it is the first object, set capacity.
    if (object_trackjectories_[cur_id].size() == 0) {
      object_trackjectories_[cur_id].set_capacity(DROPS_HISTORY_SIZE);
    }

    object_id_skip_count_[cur_id] = 0;


    object_trackjectories_[cur_id].push_back(
        std::make_pair(obj->center[0], obj->center[1]));

    if (debug_level_ >= 2) {
      AINFO << "object_trackjectories_[" << cur_id << " ].size(): "
            << object_trackjectories_[cur_id].size();
    }

//    obj->drops.clear();
    // Add drops
    for (std::size_t it = object_trackjectories_[cur_id].size() - 1, count = 0;
         it > 0; it--, count++) {
      if (count >= DROPS_HISTORY_SIZE || count > motion_buffer->size()) {
        break;
      }
      Eigen::VectorXf pt =
          Eigen::VectorXf::Zero((*motion_buffer)[0].motion.cols());
      pt[0] = object_trackjectories_[cur_id][it].first;
      pt[1] = object_trackjectories_[cur_id][it].second;
      pt[2] = 0.0f;
      pt[3] = 1.0f;

      Eigen::Vector3d transformed_pt;
      TranformPoint(pt,
                    (*motion_buffer)[motion_size - count - 1].motion,
                    &transformed_pt);
//      obj->drops.push_back(transformed_pt);
    }
  }

  // Currently remove trajectory if they do not exist in the current frame
  // TODO(techoe): need to wait several frames
  for (const auto& each_object : object_trackjectories_) {
    int obj_id = each_object.first;
    bool b_found_id = false;
    for (auto obj : *objects) {
      int cur_id = obj->track_id;
      if (obj_id == cur_id) {
        b_found_id = true;
        break;
      }
    }
    // If object ID was not found erase it from map
    if (b_found_id == false && object_trackjectories_[obj_id].size() > 0) {
//      object_id_skip_count_[obj_id].second++;
      object_id_skip_count_[obj_id]++;
      if (debug_level_ >= 2) {
        AINFO << "object_id_skip_count_[" << obj_id <<" ]: "
              << object_id_skip_count_[obj_id];
      }
      if (object_id_skip_count_[obj_id] >= MAX_ALLOWED_SKIP_OBJECT) {
        if (debug_level_ >= 2) {
          AINFO << "Removed obsolete object " << obj_id;
        }
        object_trackjectories_.erase(obj_id);
        object_id_skip_count_.erase(obj_id);
      }
    }
  }
  if (debug_level_ >= 2) {
    for (auto obj : *objects) {
      int cur_id = obj->track_id;
      AINFO << "obj->track_id: " << cur_id;
//      AINFO << "obj->drops.size(): " << obj->drops.size();
    }
  }
  return true;
}

bool Cipv::image2ground(const float image_x, const float image_y,
  float *ground_x, float *ground_y) {
  Eigen::Vector3d p_homo;

  p_homo << image_x, image_y, 1;
  Eigen::Vector3d p_ground;
  p_ground = homography_im2car_ * p_homo;
  if (fabs(p_ground[2]) > std::numeric_limits<double>::min()) {
    *ground_x = static_cast<float>(p_ground[0] / p_ground[2]);
    *ground_y = static_cast<float>(p_ground[1] / p_ground[2]);
  } else {
    if (debug_level_ >= 1) {
      AINFO << "p_ground[2] too small :" << p_ground[2];
    }
    return false;
  }
  return true;
}


bool Cipv::ground2image(const float ground_x, const float ground_y,
  float *image_x, float *image_y) {
  Eigen::Vector3d p_homo_ground;

  p_homo_ground << ground_x, ground_y, 1;
  Eigen::Vector3d p_image;
  p_image = homography_car2im_ * p_homo_ground;
  if (fabs(p_image[2]) > std::numeric_limits<double>::min()) {
    *image_x = static_cast<float>(p_image[0] / p_image[2]);
    *image_y = static_cast<float>(p_image[1] / p_image[2]);
  } else {
    if (debug_level_ >= 1) {
      AINFO << "p_image[2] too small :" << p_image[2];
    }
    return false;
  }
  return true;
}

std::string Cipv::Name() const { return "Cipv"; }

// Register plugin.
// REGISTER_CIPV(Cipv);

}  // namespace perception
}  // namespace apollo
