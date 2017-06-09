/*
Copyright 2017 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS-IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef LULLABY_SYSTEMS_DISPATCHER_DISPATCHER_SYSTEM_H_
#define LULLABY_SYSTEMS_DISPATCHER_DISPATCHER_SYSTEM_H_

#include "lullaby/generated/dispatcher_def_generated.h"
#include "lullaby/base/dispatcher.h"
#include "lullaby/base/system.h"
#include "lullaby/base/thread_safe_queue.h"

namespace lull {

// Provides a Dispatcher as a Component for each Entity.
class DispatcherSystem : public System {
 public:
  static void EnableQueuedDispatch();
  static void DisableQueuedDispatch();

  explicit DispatcherSystem(Registry* registry);

  ~DispatcherSystem() override;

  // Associates EventResponses with the Entity based on the |def|.
  void Create(Entity entity, HashValue type, const Def* def) override;

  // Destroys the Dispatcher and any Connections associated with the Entity.
  void Destroy(Entity entity) override;

  // Sends |event| to all functions registered with the dispatcher associated
  // with |entity|.  The |Event| type must be registered with
  // LULLABY_SETUP_TYPEID.
  template <typename Event>
  void Send(Entity entity, const Event& event) {
    SendImpl(entity, EventWrapper(event));
  }

  void Send(Entity entity, const EventWrapper& event_wrapper) {
    SendImpl(entity, event_wrapper);
  }

  // As Send, but will always send immediately regardless of QueuedDispatch
  // setting.
  template <typename Event>
  void SendImmediately(Entity entity, const Event& event) {
    SendImmediatelyImpl(entity, EventWrapper(event));
  }

  void SendImmediately(Entity entity, const EventWrapper& event_wrapper) {
    SendImmediatelyImpl(entity, event_wrapper);
  }

  // Dispatches all events currently queued in the DispatcherSystem.
  void Dispatch();

  // Connects an event handler to the Dispatcher associated with |entity|.  This
  // function is a simple wrapper around the various Dispatcher::Connect
  // functions.  For more information, please refer to the Dispatcher API.
  template <typename... Args>
  auto Connect(Entity entity, Args&&... args) -> decltype(
      std::declval<Dispatcher>().Connect(std::forward<Args>(args)...)) {
    Dispatcher* dispatcher = GetDispatcher(entity);
    if (dispatcher) {
      return dispatcher->Connect(std::forward<Args>(args)...);
    } else {
      return Dispatcher::Connection();
    }
  }

  // Connects the |handler| to an event as described by the |input|.
  void ConnectEvent(Entity entity, const EventDef* input,
                    Dispatcher::EventHandler handler);

  // Disconnects an event handler identified by the |owner| from the Dispatcher
  // associated with |entity|.  See Dispatcher::Disconnect for more information.
  template <typename Event>
  void Disconnect(Entity entity, const void* owner) {
    Disconnect(entity, GetTypeId<Event>(), owner);
  }

  // Disconnects an event handler identified by the |owner| from the Dispatcher
  // associated with |entity|.  See Dispatcher::Disconnect for more information.
  void Disconnect(Entity entity, TypeId type, const void* owner);

 private:
  struct EntityEvent {
    Entity entity;
    std::unique_ptr<EventWrapper> event;
  };

  using EventQueue = ThreadSafeQueue<EntityEvent>;
  using EntityDispatcherMap = std::unordered_map<Entity, Dispatcher>;
  using EntityConnections =
      std::unordered_map<Entity, std::vector<Dispatcher::ScopedConnection>>;

  void SendImpl(Entity entity, const EventWrapper& event);
  void SendImmediatelyImpl(Entity entity, const EventWrapper& event);

  Dispatcher* GetDispatcher(Entity entity);

  EventQueue queue_;
  EntityConnections connections_;
  EntityDispatcherMap dispatchers_;
  static bool enable_queued_dispatch_;

  DispatcherSystem(const DispatcherSystem&);
  DispatcherSystem& operator=(const DispatcherSystem&);
};

}  // namespace lull

LULLABY_SETUP_TYPEID(lull::DispatcherSystem);

#endif  // LULLABY_SYSTEMS_DISPATCHER_DISPATCHER_SYSTEM_H_
