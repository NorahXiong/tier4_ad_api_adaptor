// Copyright 2021 TIER IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "operator.hpp"

#include <memory>

namespace internal_api
{
Operator::Operator(const rclcpp::NodeOptions & options) : Node("external_api_operator", options)
{
  using namespace std::literals::chrono_literals;
  using std::placeholders::_1;
  using std::placeholders::_2;
  tier4_api_utils::ServiceProxyNodeInterface proxy(this);

  group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  srv_set_operator_ = proxy.create_service<tier4_external_api_msgs::srv::SetOperator>(
    "/api/autoware/set/operator", std::bind(&Operator::setOperator, this, _1, _2),
    rmw_qos_profile_services_default, group_);
  srv_set_observer_ = proxy.create_service<tier4_external_api_msgs::srv::SetObserver>(
    "/api/autoware/set/observer", std::bind(&Operator::setObserver, this, _1, _2),
    rmw_qos_profile_services_default, group_);

  cli_external_select_ = proxy.create_client<tier4_control_msgs::srv::ExternalCommandSelect>(
    "/control/external_cmd_selector/select_external_command");
  cli_operation_mode_ = proxy.create_client<tier4_system_msgs::srv::OperationModeRequest>(
    "/system/operation_mode_request");
  pub_gate_mode_ =
    create_publisher<tier4_control_msgs::msg::GateMode>("/control/gate_mode_cmd", rclcpp::QoS(1));
  pub_vehicle_engage_ =
    create_publisher<autoware_auto_vehicle_msgs::msg::Engage>("/vehicle/engage", rclcpp::QoS(1));

  pub_operator_ = create_publisher<tier4_external_api_msgs::msg::Operator>(
    "/api/autoware/get/operator", rclcpp::QoS(1));
  pub_observer_ = create_publisher<tier4_external_api_msgs::msg::Observer>(
    "/api/autoware/get/observer", rclcpp::QoS(1));

  sub_external_select_ = create_subscription<tier4_control_msgs::msg::ExternalCommandSelectorMode>(
    "/control/external_cmd_selector/current_selector_mode", rclcpp::QoS(1),
    std::bind(&Operator::onExternalSelect, this, _1));
  sub_gate_mode_ = create_subscription<tier4_control_msgs::msg::GateMode>(
    "/control/current_gate_mode", rclcpp::QoS(1), std::bind(&Operator::onGateMode, this, _1));
  sub_vehicle_control_mode_ =
    create_subscription<autoware_auto_vehicle_msgs::msg::ControlModeReport>(
      "/vehicle/status/control_mode", rclcpp::QoS(1),
      std::bind(&Operator::onVehicleControlMode, this, _1));

  timer_ = rclcpp::create_timer(this, get_clock(), 200ms, std::bind(&Operator::onTimer, this));
}

void Operator::setOperator(
  const tier4_external_api_msgs::srv::SetOperator::Request::SharedPtr request,
  const tier4_external_api_msgs::srv::SetOperator::Response::SharedPtr response)
{
  switch (request->mode.mode) {
    case tier4_external_api_msgs::msg::Operator::DRIVER:
      setVehicleEngage(false);  // TODO(horibe): keep for backward compatibility. Will be removed.
      response->status = setVehicleOperationMode(OperationMode::MANUAL_DIRECT);
      return;

    case tier4_external_api_msgs::msg::Operator::AUTONOMOUS:
      setGateMode(tier4_control_msgs::msg::GateMode::AUTO);
      setVehicleEngage(true);  // TODO(horibe): keep for backward compatibility. Will be removed.
      response->status = setVehicleOperationMode(OperationMode::AUTONOMOUS);
      return;

    case tier4_external_api_msgs::msg::Operator::OBSERVER:
      // TODO(Takagi, Isamu): prohibit transition when none observer type is added
      setGateMode(tier4_control_msgs::msg::GateMode::EXTERNAL);
      setVehicleEngage(true);  // TODO(horibe): keep for backward compatibility. Will be removed.
      response->status = setVehicleOperationMode(OperationMode::REMOTE_OPERATOR);
      return;

    default:
      response->status = tier4_api_utils::response_error("Invalid parameter.");
      return;
  }
}

void Operator::setObserver(
  const tier4_external_api_msgs::srv::SetObserver::Request::SharedPtr request,
  const tier4_external_api_msgs::srv::SetObserver::Response::SharedPtr response)
{
  using ExternalCommandSelectorMode = tier4_control_msgs::msg::ExternalCommandSelectorMode;

  switch (request->mode.mode) {
    case tier4_external_api_msgs::msg::Observer::LOCAL:
      response->status = setExternalSelect(ExternalCommandSelectorMode::LOCAL);
      return;

    case tier4_external_api_msgs::msg::Observer::REMOTE:
      response->status = setExternalSelect(ExternalCommandSelectorMode::REMOTE);
      return;

    default:
      response->status = tier4_api_utils::response_error("Invalid parameter.");
      return;
  }
}

void Operator::onExternalSelect(
  const tier4_control_msgs::msg::ExternalCommandSelectorMode::ConstSharedPtr message)
{
  external_select_ = message;
}

void Operator::onGateMode(const tier4_control_msgs::msg::GateMode::ConstSharedPtr message)
{
  gate_mode_ = message;
}

void Operator::onVehicleControlMode(
  const autoware_auto_vehicle_msgs::msg::ControlModeReport::ConstSharedPtr message)
{
  vehicle_control_mode_ = message;
}

void Operator::onTimer()
{
  publishOperator();
  publishObserver();
}

void Operator::publishOperator()
{
  using OperatorMsg = tier4_external_api_msgs::msg::Operator;
  using tier4_external_api_msgs::build;

  if (!vehicle_control_mode_ || !gate_mode_) {
    return;
  }

  if (vehicle_control_mode_->mode == autoware_auto_vehicle_msgs::msg::ControlModeReport::MANUAL) {
    pub_operator_->publish(build<OperatorMsg>().mode(OperatorMsg::DRIVER));
    return;
  }
  switch (gate_mode_->data) {
    case tier4_control_msgs::msg::GateMode::AUTO:
      pub_operator_->publish(build<OperatorMsg>().mode(OperatorMsg::AUTONOMOUS));
      return;

    case tier4_control_msgs::msg::GateMode::EXTERNAL:
      pub_operator_->publish(build<OperatorMsg>().mode(OperatorMsg::OBSERVER));
      return;
  }
  RCLCPP_ERROR(get_logger(), "Unknown operator.");
}

void Operator::publishObserver()
{
  using ObserverMsg = tier4_external_api_msgs::msg::Observer;
  using tier4_external_api_msgs::build;

  if (!external_select_) {
    return;
  }

  switch (external_select_->data) {
    case tier4_control_msgs::msg::ExternalCommandSelectorMode::LOCAL:
      pub_observer_->publish(build<ObserverMsg>().mode(ObserverMsg::LOCAL));
      return;

    case tier4_control_msgs::msg::ExternalCommandSelectorMode::REMOTE:
      pub_observer_->publish(build<ObserverMsg>().mode(ObserverMsg::REMOTE));
      return;
  }
  RCLCPP_ERROR(get_logger(), "Unknown observer.");
}

void Operator::setVehicleEngage(bool engage)
{
  const auto msg = autoware_auto_vehicle_msgs::build<autoware_auto_vehicle_msgs::msg::Engage>()
                     .stamp(now())
                     .engage(engage);
  pub_vehicle_engage_->publish(msg);
}

tier4_external_api_msgs::msg::ResponseStatus Operator::setVehicleOperationMode(uint8_t mode)
{
  const auto req = std::make_shared<tier4_system_msgs::srv::OperationModeRequest::Request>();
  req->mode.mode = mode;

  const auto [status, resp] = cli_operation_mode_->call(req);
  if (!tier4_api_utils::is_success(status)) {
    return status;
  }

  if (resp->success) {
    return tier4_api_utils::response_success("set OperationMode succeeded");
  } else {
    return tier4_api_utils::response_error("set OperationMode failed.");
  }
}

void Operator::setGateMode(tier4_control_msgs::msg::GateMode::_data_type data)
{
  const auto msg = tier4_control_msgs::build<tier4_control_msgs::msg::GateMode>().data(data);
  pub_gate_mode_->publish(msg);
}

tier4_external_api_msgs::msg::ResponseStatus Operator::setExternalSelect(
  tier4_control_msgs::msg::ExternalCommandSelectorMode::_data_type data)
{
  const auto req = std::make_shared<tier4_control_msgs::srv::ExternalCommandSelect::Request>();
  req->mode.data = data;

  const auto [status, resp] = cli_external_select_->call(req);
  if (!tier4_api_utils::is_success(status)) {
    return status;
  }

  if (resp->success) {
    return tier4_api_utils::response_success(resp->message);
  } else {
    return tier4_api_utils::response_error(resp->message);
  }
}

}  // namespace internal_api

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(internal_api::Operator)
