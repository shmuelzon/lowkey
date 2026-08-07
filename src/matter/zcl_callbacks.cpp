#include "matter_radio.h"
#include "lock_manager.h"
#include "door_monitor.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <lib/support/CodeUtils.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app::Clusters;
using namespace ::chip::app::Clusters::DoorLock;

void MatterPostAttributeChangeCallback(
	const chip::app::ConcreteAttributePath &attributePath, uint8_t type,
	uint16_t size, uint8_t *value)
{
	/*
	 * This lock has no PIN/RFID credentials, so there are no dynamic
	 * attribute responses to handle (e.g. RequirePINforRemoteOperation).
	 */
}

/*
 * User and credential management stubs.  This lock has no PIN/RFID
 * support — all user/credential queries return empty/unsupported.
 */
bool emberAfPluginDoorLockGetUser(EndpointId endpointId,
				  uint16_t userIndex,
				  EmberAfPluginDoorLockUserInfo &user)
{
	return false;
}

bool emberAfPluginDoorLockSetUser(EndpointId endpointId,
				  uint16_t userIndex,
				  chip::FabricIndex creator,
				  chip::FabricIndex modifier,
				  const chip::CharSpan &userName,
				  uint32_t uniqueId,
				  UserStatusEnum userStatus,
				  UserTypeEnum userType,
				  CredentialRuleEnum credentialRule,
				  const CredentialStruct *credentials,
				  size_t totalCredentials)
{
	return false;
}

bool emberAfPluginDoorLockGetCredential(
	EndpointId endpointId, uint16_t credentialIndex,
	CredentialTypeEnum credentialType,
	EmberAfPluginDoorLockCredentialInfo &credential)
{
	return false;
}

bool emberAfPluginDoorLockSetCredential(
	EndpointId endpointId, uint16_t credentialIndex,
	chip::FabricIndex creator, chip::FabricIndex modifier,
	DlCredentialStatus credentialStatus,
	CredentialTypeEnum credentialType,
	const chip::ByteSpan &secret)
{
	return false;
}

/*
 * Shared body of the three remote lock commands, which differ only in the
 * action they ask the hardware to perform.
 *
 * fabricIdx/nodeId are carried down so the resulting LockOperation event names
 * the client that operated the lock; a controller uses them to say "Alice
 * unlocked the door" instead of attributing it to nobody. Both halves are
 * needed — the cluster logs an error for a remote operation missing either.
 */
static bool handle_remote_request(
	enum lock_action action,
	const chip::app::DataModel::Nullable<chip::FabricIndex> &fabricIdx,
	const chip::app::DataModel::Nullable<chip::NodeId> &nodeId,
	OperationErrorEnum &err)
{
	struct lock_originator originator = {};

	if (!fabricIdx.IsNull() && !nodeId.IsNull()) {
		originator.valid = true;
		originator.fabric_index = fabricIdx.Value();
		originator.node_id = nodeId.Value();
	}

	int ret = matter_radio_lock_request(action, LOCK_OP_SOURCE_REMOTE,
					    &originator);

	/* Already in (or already moving to) the requested position. */
	if (ret == -EALREADY) {
		return true;
	}

	if (ret) {
		err = OperationErrorEnum::kUnspecified;
		return false;
	}

	return true;
}

bool emberAfPluginDoorLockOnDoorLockCommand(
	EndpointId endpointId,
	const chip::app::DataModel::Nullable<chip::FabricIndex> &fabricIdx,
	const chip::app::DataModel::Nullable<chip::NodeId> &nodeId,
	const Optional<ByteSpan> &pinCode, OperationErrorEnum &err)
{
	return handle_remote_request(LOCK_ACTION_LOCK, fabricIdx, nodeId, err);
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(
	EndpointId endpointId,
	const chip::app::DataModel::Nullable<chip::FabricIndex> &fabricIdx,
	const chip::app::DataModel::Nullable<chip::NodeId> &nodeId,
	const Optional<ByteSpan> &pinCode, OperationErrorEnum &err)
{
	/*
	 * UnlockDoor on a lock that advertises Unbolting is the unlatch
	 * operation: retract the bolt AND pull the latch, so the door can
	 * actually be pushed open. The cluster agrees — with the feature set,
	 * emberAfDoorLockClusterUnlockDoorCallback logs the operation as
	 * Unlatch, and it expects the resulting LockState to be Unlatched.
	 * UnlockWithTimeout routes here too, for the same reason.
	 */
	return handle_remote_request(LOCK_ACTION_OPEN, fabricIdx, nodeId, err);
}

bool emberAfPluginDoorLockOnDoorUnboltCommand(
	EndpointId endpointId,
	const chip::app::DataModel::Nullable<chip::FabricIndex> &fabricIdx,
	const chip::app::DataModel::Nullable<chip::NodeId> &nodeId,
	const Optional<ByteSpan> &pinCode, OperationErrorEnum &err)
{
	/*
	 * UnboltDoor is the narrower operation: retract the bolt and stop,
	 * leaving the spring latch to hold the door shut. Resulting LockState is
	 * Unlocked, not Unlatched.
	 */
	return handle_remote_request(LOCK_ACTION_UNLOCK, fabricIdx, nodeId, err);
}

void emberAfPluginDoorLockOnAutoRelock(chip::EndpointId endpointId)
{
	/* Nobody asked for this one, so there is no originator to report. */
	matter_radio_lock_request(LOCK_ACTION_LOCK, LOCK_OP_SOURCE_AUTO, NULL);
}

void emberAfDoorLockClusterInitCallback(EndpointId endpoint)
{
	DoorLockServer::Instance().InitServer(endpoint);

	const auto logOnFailure = [](Protocols::InteractionModel::Status status,
				     const char *name) {
		if (status != Protocols::InteractionModel::Status::Success) {
			ChipLogError(Zcl, "Failed to set DoorLock %s: %x",
				     name, to_underlying(status));
		}
	};

	logOnFailure(Attributes::LockType::Set(endpoint,
					       DlLockType::kDeadBolt),
		     "type");
	logOnFailure(Attributes::ActuatorEnabled::Set(endpoint, true),
		     "actuator enabled");

	/*
	 * This lock exposes no PIN/RFID/User features (see lock.zap FeatureMap
	 * 0x1020 = Unbolt | DoorPositionSensor), so the user/credential count
	 * attributes are not part of the data model and must not be written.
	 */

	enum lock_state ls = lock_manager_get_state();
	enum door_state ds = door_monitor_get_state();

	matter_radio_publish_initial_state(ls, ds);
}
