#pragma once
#include "model.hpp"
#include <string_view>
namespace voyemu {
// The vehicle gateway contract of the Passion cluster (see SomeipGateway), read from its pinned
// binaries at preparation: services and VLANs from the generated SOME/IP clients of
// /usr/bin/ic_service (each <Service>_client_data and the VLAN name its start_<Service>_client call
// passes), instances, versions, eventgroups and event ids from its event_field_notifier_* records,
// and the payload of each event from the SOME/IP service table: its signals in order, packed
// big-endian, named as in the Passion vehicle catalogue (by position where the table disagrees).
// Field getters (field_getter_*_rpc_call) give each service its method ids and response layouts.
Json qnx_passion_someip_contract(std::string_view ic_service, const Json &service_table);
} // namespace voyemu
