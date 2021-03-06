/* Copyright 2016 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "legion.h"
#include "runtime.h"
#include "legion_ops.h"
#include "legion_tasks.h"
#include "region_tree.h"
#include "legion_spy.h"
#include "legion_profiling.h"
#include "legion_instances.h"
#include "legion_views.h"
#include "legion_analysis.h"
#include "interval_tree.h"
#include "rectangle_set.h"

namespace Legion {
  namespace Internal {

    LEGION_EXTERN_LOGGER_DECLARATIONS

    /////////////////////////////////////////////////////////////
    // Region Tree Forest 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    RegionTreeForest::RegionTreeForest(Runtime *rt)
      : runtime(rt)
    //--------------------------------------------------------------------------
    {
      this->lookup_lock = Reservation::create_reservation();
    }

    //--------------------------------------------------------------------------
    RegionTreeForest::RegionTreeForest(const RegionTreeForest &rhs)
      : runtime(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    RegionTreeForest::~RegionTreeForest(void)
    //--------------------------------------------------------------------------
    {
      lookup_lock.destroy_reservation();
      lookup_lock = Reservation::NO_RESERVATION;
    }

    //--------------------------------------------------------------------------
    RegionTreeForest& RegionTreeForest::operator=(const RegionTreeForest &rhs)
    //--------------------------------------------------------------------------
    {
      // should never happen
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::create_index_space(IndexSpace handle,
                                              const Domain &domain,
                                              IndexSpaceKind kind,
                                              AllocateMode mode) 
    //--------------------------------------------------------------------------
    {
      create_node(handle, domain, NULL/*parent*/, 
                  ColorPoint(0)/*color*/, kind, mode);
      if (Runtime::legion_spy_enabled)
        IndexSpaceNode::log_index_space_domain(handle, domain);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::create_index_space(IndexSpace handle,
                                              const Domain &hull,
                                              const std::set<Domain> &domains,
                                              IndexSpaceKind kind,
                                              AllocateMode mode)
    //--------------------------------------------------------------------------
    {
      // Note that it is safe that we do this in two passes
      // because we haven't given back the handle yet for
      // the index space so no one actually knows it exists yet.
      IndexSpaceNode *node = create_node(handle, hull, NULL/*parent*/, 
                                         ColorPoint(0)/*color*/, kind, mode);
      node->update_component_domains(domains);
      if (Runtime::legion_spy_enabled)
      {
        for (std::set<Domain>::const_iterator it = domains.begin();
              it != domains.end(); it++)
          IndexSpaceNode::log_index_space_domain(handle, *it);
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::create_index_partition(IndexPartition pid,
        IndexSpace parent, ColorPoint part_color, 
        const std::map<DomainPoint,Domain> &coloring, 
        const Domain &color_space, PartitionKind part_kind, AllocateMode mode)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *parent_node = get_node(parent);
      if (!part_color.is_valid())
        part_color = ColorPoint(DomainPoint::from_point<1>(
              LegionRuntime::Arrays::Point<1>(parent_node->generate_color())));
      // If we are making this partition on a different node than the
      // owner node of the parent index space then we have to tell that
      // owner node about the existence of this partition
      RtEvent parent_notified;
      const AddressSpaceID parent_owner = parent_node->get_owner_space();
      // If we have to send the notification and we know the disjointness
      // then we can send it now, otherwise, wait for disjointness test
      if ((parent_owner != runtime->address_space) && 
          (part_kind != COMPUTE_KIND))
      {
        RtUserEvent notified_event = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(pid);
          rez.serialize(parent);
          rez.serialize(part_color);
          rez.serialize(color_space);
          rez.serialize(part_kind);
          rez.serialize(mode);
          rez.serialize(notified_event);
        }
        runtime->send_index_partition_notification(parent_owner, rez);
        parent_notified = notified_event;
      }
      IndexPartNode *new_part;
      RtUserEvent disjointness_event;
      if (part_kind == COMPUTE_KIND)
      {
        disjointness_event = Runtime::create_rt_user_event();
        new_part = create_node(pid, parent_node, part_color, color_space,
                               disjointness_event, mode);
      }
      else
        new_part = create_node(pid, parent_node, part_color, color_space, 
                               (part_kind == DISJOINT_KIND), mode);

      if (Runtime::legion_spy_enabled)
      {
        bool disjoint = (part_kind == DISJOINT_KIND);
        LegionSpy::log_index_partition(parent.id, pid.id, disjoint,
            part_color.get_point());
      }
      // Now do all the child nodes
      for (std::map<DomainPoint,Domain>::const_iterator it = 
            coloring.begin(); it != coloring.end(); it++)
      {
        if (!color_space.contains(it->first))
        {
          log_index.error("Invalid child color specified "
                                "for create index partition.  All colors "
                                "must be contained within the "
                                "given color space");
#ifdef DEBUG_LEGION
          assert(false);
#endif
          exit(ERROR_INVALID_PARTITION_COLOR);
        }
        IndexSpace handle(runtime->get_unique_index_space_id(),
                          pid.get_tree_id());
        create_node(handle, it->second, new_part, ColorPoint(it->first),
                    parent_node->kind, mode);

        if (Runtime::legion_spy_enabled)
        {
          LegionSpy::log_index_subspace(pid.id, handle.id, it->first);
          IndexSpaceNode::log_index_space_domain(handle, it->second);
        }
      } 
      if (part_kind == COMPUTE_KIND)
      {
#ifdef DEBUG_LEGION
        assert(disjointness_event.exists());
#endif
        // Launch a task to compute the disjointness
        DisjointnessArgs args;
        args.hlr_id = HLR_DISJOINTNESS_TASK_ID;
        args.handle = pid;
        args.ready = disjointness_event;
        runtime->issue_runtime_meta_task(&args, sizeof(args),
                                         HLR_DISJOINTNESS_TASK_ID,
                                         HLR_LATENCY_PRIORITY);
      }
      // If we didn't send the notification yet because we need the 
      // disjointness test, now we can send it
      if ((parent_owner != runtime->address_space) && 
          (part_kind == COMPUTE_KIND))
      {
        // Get the disjointness result, this will likely wait on
        // the disjointness test issued above
        const bool disjoint = new_part->is_disjoint(true/*app*/);
        RtUserEvent notified_event = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(pid);
          rez.serialize(parent);
          rez.serialize(part_color);
          rez.serialize(color_space);
          if (disjoint)
            rez.serialize(DISJOINT_KIND);
          else
            rez.serialize(ALIASED_KIND);
          rez.serialize(mode);
          rez.serialize(notified_event);
        }
        runtime->send_index_partition_notification(parent_owner, rez);
        parent_notified = notified_event;
      }
      // Wait for the parent index space to be notified if necessary
      if (parent_notified.exists())
        parent_notified.wait();
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::create_index_partition(IndexPartition pid,
       IndexSpace parent, ColorPoint part_color, 
       const std::map<DomainPoint,Domain> &convex_hulls,
       const std::map<DomainPoint,std::set<Domain> > &component_domains,
       const Domain &color_space, PartitionKind part_kind, AllocateMode mode)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *parent_node = get_node(parent);
      if (!part_color.is_valid())
        part_color = ColorPoint(DomainPoint::from_point<1>(
              LegionRuntime::Arrays::Point<1>(parent_node->generate_color())));
      // If we are making this partition on a different node than the
      // owner node of the parent index space then we have to tell that
      // owner node about the existence of this partition
      RtEvent parent_notified;
      const AddressSpaceID parent_owner = parent_node->get_owner_space();
      // If we know the disjointness result, we can send the notification
      // now otherwise we will wait until we know the disjointness
      if ((parent_owner != runtime->address_space) &&
          (part_kind != COMPUTE_KIND))
      {
        RtUserEvent notified_event = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(pid);
          rez.serialize(parent);
          rez.serialize(part_color);
          rez.serialize(color_space);
          rez.serialize(part_kind);
          rez.serialize(mode);
          rez.serialize(notified_event);
        }
        runtime->send_index_partition_notification(parent_owner, rez);
        parent_notified = notified_event;
      }
      IndexPartNode *new_part;
      RtUserEvent disjointness_event;
      if (part_kind == COMPUTE_KIND)
      {
        disjointness_event = Runtime::create_rt_user_event();
        new_part = create_node(pid, parent_node, part_color, color_space, 
                               disjointness_event, mode);
      }
      else
        new_part = create_node(pid, parent_node, part_color, color_space, 
                               (part_kind == DISJOINT_KIND), mode);

      if (Runtime::legion_spy_enabled)
      {
        bool disjoint = (part_kind == DISJOINT_KIND);
        LegionSpy::log_index_partition(parent.id, pid.id, disjoint,
            part_color.get_point());
      }
      // Now do all the child nodes
      std::map<DomainPoint,std::set<Domain> >::const_iterator comp_it = 
        component_domains.begin();
      for (std::map<DomainPoint,Domain>::const_iterator it = 
            convex_hulls.begin(); it != convex_hulls.end(); it++, comp_it++)
      {
        if (!color_space.contains(it->first))
        {
          log_index.error("Invalid child color specified "
                                "for create index partition.  All colors "
                                "must be contained within the given"
                                "color space");
#ifdef DEBUG_LEGION
          assert(false);
#endif
          exit(ERROR_INVALID_PARTITION_COLOR);
        }
        IndexSpace handle(runtime->get_unique_index_space_id(),
                          pid.get_tree_id());
        IndexSpaceNode *child = create_node(handle, it->second, 
                                            new_part, ColorPoint(it->first),
                                            parent_node->kind, mode);
        child->update_component_domains(comp_it->second);

        if (Runtime::legion_spy_enabled)
        {
          LegionSpy::log_index_subspace(pid.id, handle.id, it->first);
          for (std::set<Domain>::const_iterator cit = 
                comp_it->second.begin(); cit != comp_it->second.end(); cit++)
            IndexSpaceNode::log_index_space_domain(handle, *cit); 
        }
      }
      if (part_kind == COMPUTE_KIND)
      {
#ifdef DEBUG_LEGION
        assert(disjointness_event.exists());
#endif
        // Launch a task to compute the disjointness
        DisjointnessArgs args;
        args.hlr_id = HLR_DISJOINTNESS_TASK_ID;
        args.handle = pid;
        args.ready = disjointness_event;
        runtime->issue_runtime_meta_task(&args, sizeof(args),
                                         HLR_DISJOINTNESS_TASK_ID,
                                         HLR_LATENCY_PRIORITY);
      }
      // If we didn't send the notification yet because we need the 
      // disjointness test, now we can send it
      if ((parent_owner != runtime->address_space) && 
          (part_kind == COMPUTE_KIND))
      {
        // Get the disjointness result, this will likely wait on
        // the disjointness test issued above
        const bool disjoint = new_part->is_disjoint(true/*app*/);
        RtUserEvent notified_event = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(pid);
          rez.serialize(parent);
          rez.serialize(part_color);
          rez.serialize(color_space);
          if (disjoint)
            rez.serialize(DISJOINT_KIND);
          else
            rez.serialize(ALIASED_KIND);
          rez.serialize(mode);
          rez.serialize(notified_event);
        }
        runtime->send_index_partition_notification(parent_owner, rez);
        parent_notified = notified_event;
      }
      // Wait for the parent index space to be notified if necessary
      if (parent_notified.exists())
        parent_notified.wait();
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::compute_partition_disjointness(IndexPartition handle,
                                                        RtUserEvent ready_event)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *node = get_node(handle);
      node->compute_disjointness(ready_event);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::destroy_index_space(IndexSpace handle,
                                               AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(handle);
      node->destroy_node(source);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::destroy_index_partition(IndexPartition handle,
                                                   AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *node = get_node(handle);
      node->destroy_node(source);
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::create_equal_partition(IndexPartition pid,
                                                     size_t granularity)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *new_part = get_node(pid);
      return new_part->create_equal_children(granularity);
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::create_weighted_partition(IndexPartition pid,
                                                        size_t granularity,
                                       const std::map<DomainPoint,int> &weights)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *new_part = get_node(pid);
      return new_part->create_weighted_children(weights, granularity);
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::create_partition_by_union(IndexPartition pid,
                                                        IndexPartition handle1,
                                                        IndexPartition handle2)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *new_part = get_node(pid);
      IndexPartNode *node1 = get_node(handle1);
      IndexPartNode *node2 = get_node(handle2);
      return new_part->create_by_operation(node1, node2,
                                           Realm::IndexSpace::ISO_UNION);
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::create_partition_by_intersection(
                                                         IndexPartition pid,
                                                         IndexPartition handle1,
                                                         IndexPartition handle2)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *new_part = get_node(pid);
      IndexPartNode *node1 = get_node(handle1);
      IndexPartNode *node2 = get_node(handle2);
      return new_part->create_by_operation(node1, node2,
                                           Realm::IndexSpace::ISO_INTERSECT);
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::create_partition_by_difference(
                                                       IndexPartition pid,
                                                       IndexPartition handle1,
                                                       IndexPartition handle2)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *new_part = get_node(pid);
      IndexPartNode *node1 = get_node(handle1);
      IndexPartNode *node2 = get_node(handle2);
      return new_part->create_by_operation(node1, node2,
                                           Realm::IndexSpace::ISO_SUBTRACT);
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::create_cross_product_partitions(
                                                          IndexPartition base,
                                                          IndexPartition source,
                                  std::map<DomainPoint,IndexPartition> &handles)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *base_node = get_node(base);
      IndexPartNode *source_node = get_node(source);
      std::set<ApEvent> ready_events;
      // Iterate over all our sub-regions and fill in the intersections
      for (std::map<DomainPoint,IndexPartition>::const_iterator it = 
            handles.begin(); it != handles.end(); it++)
      {
        ColorPoint child_color(it->first);
        IndexSpaceNode *child_node = base_node->get_child(child_color);
        IndexPartNode *part_node = get_node(it->second);
        ApEvent ready = part_node->create_by_operation(child_node, source_node,
                                        Realm::IndexSpace::ISO_INTERSECT);
        ready_events.insert(ready);
      }
      return Runtime::merge_events(ready_events);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::compute_pending_color_space(IndexSpace parent,
                                                       IndexPartition handle1,
                                                       IndexPartition handle2,
                                                       Domain &color_space,
                                   Realm::IndexSpace::IndexSpaceOperation op)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      std::vector<ColorPoint> path;
      switch (op)
      {
        case Realm::IndexSpace::ISO_UNION:
          {
            // Check that parent is an ancestor of both partitions
            if (!compute_partition_path(parent, handle1, path))
            {
              log_index.error("Index space %d is not an ancestor of "
                                    "index partition %d in create partition "
                                    "by union call!", parent.id, handle1.id);
              assert(false);
              exit(ERROR_INDEX_PARTITION_ANCESTOR);
            }
            path.clear();
            if (!compute_partition_path(parent, handle2, path))
            {
              log_index.error("Index space %d is not an ancestor of "
                                    "index partition %d in create partition "
                                    "by union call!", parent.id, handle1.id);
              assert(false);
              exit(ERROR_INDEX_PARTITION_ANCESTOR);
            }
            break;
          }
        case Realm::IndexSpace::ISO_INTERSECT:
          {
            // Check that parent is an ancestor of one of the partitions
            if (!compute_partition_path(parent, handle1, path))
            {
              path.clear();
              if (!compute_partition_path(parent, handle2, path))
              {
                log_index.error("Index space %d is not an ancestor of "
                                      "either index partition %d or index "
                                      "partition %d in create partition by "
                                      "intersection call!", 
                                      parent.id, handle1.id, handle2.id);
                assert(false);
                exit(ERROR_INDEX_PARTITION_ANCESTOR);
              }
            }
            break;
          }
        case Realm::IndexSpace::ISO_SUBTRACT:
          {
            // Check that the parent is an ancestor of the first index partition
            if (!compute_partition_path(parent, handle1, path))
            {
              log_index.error("Index space %d is not an ancestor of "
                                    "index partition %d in create partition "
                                    "by difference call!", 
                                    parent.id, handle1.id);
              assert(false);
              exit(ERROR_INDEX_PARTITION_ANCESTOR);
            }
            break;
          }
        default:
          assert(false); // should never get here
      }
#endif
      IndexPartNode *node1 = get_node(handle1);
      IndexPartNode *node2 = get_node(handle2);
      // Compute the color space
      IndexTreeNode::compute_intersection(node1->color_space, 
                                          node2->color_space,
                                          color_space, true/*compute*/);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::create_pending_partition(IndexPartition pid,
                                                    IndexSpace parent,
                                                    const Domain &color_space,
                                                    ColorPoint partition_color,
                                                    PartitionKind part_kind,
                                                    bool allocable,
                                                    ApEvent handle_ready,
                                                    ApEvent domain_ready,
                                                    bool create_separate)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *parent_node = get_node(parent);
      if (!partition_color.is_valid())
        partition_color = ColorPoint(DomainPoint::from_point<1>(
              LegionRuntime::Arrays::Point<1>(parent_node->generate_color())));
      RtUserEvent disjointness_event;
      IndexPartNode *partition_node;
      if (part_kind == COMPUTE_KIND)
      {
        disjointness_event = Runtime::create_rt_user_event();
        partition_node = create_node(pid, parent_node, partition_color,
                                     color_space, disjointness_event,
                                     allocable ? MUTABLE : NO_MEMORY);
      }
      else
        partition_node = create_node(pid, parent_node, partition_color,
                                     color_space, (part_kind == DISJOINT_KIND),
                                     allocable ? MUTABLE : NO_MEMORY);

      if (Runtime::legion_spy_enabled)
      {
        bool disjoint = (part_kind == DISJOINT_KIND);
        LegionSpy::log_index_partition(parent.id, pid.id, disjoint,
            partition_color.get_point());
      }
      // We also need to explicitly instantiate all the children so
      // that they know the domains will be ready at a later time.
      // We instantiate them with an empty domain that will be filled in later
      for (Domain::DomainPointIterator itr(color_space); itr; itr++)
      {
        IndexSpace is(runtime->get_unique_index_space_id(), pid.get_tree_id());
        ColorPoint child_color(itr.p);
        if (create_separate)
        {
#ifdef DEBUG_LEGION
          assert(!handle_ready.exists());
          assert(!domain_ready.exists());
#endif
          // Create a separate handle ready event for each node
          ApUserEvent local_handle_ready = Runtime::create_ap_user_event();
          ApUserEvent local_domain_ready = Runtime::create_ap_user_event();
          create_node(is, local_handle_ready, local_domain_ready,
                      partition_node, child_color, parent_node->kind, 
                      allocable ? MUTABLE : NO_MEMORY);
          partition_node->add_pending_child(child_color, local_handle_ready,
                                            local_domain_ready);
        }
        else
          create_node(is, handle_ready, domain_ready,
                      partition_node, child_color, parent_node->kind, 
                      allocable ? MUTABLE : NO_MEMORY);
        if (Runtime::legion_spy_enabled)
          LegionSpy::log_index_subspace(pid.id, is.id, itr.p);
      }
      // If we need to compute the disjointness, only do that
      // after the partition is actually ready
      if (part_kind == COMPUTE_KIND)
      {
#ifdef DEBUG_LEGION
        assert(disjointness_event.exists());
#endif
        // Launch a task to compute the disjointness
        DisjointnessArgs args;
        args.hlr_id = HLR_DISJOINTNESS_TASK_ID;
        args.handle = pid;
        args.ready = disjointness_event;
        runtime->issue_runtime_meta_task(&args, sizeof(args),
                                         HLR_DISJOINTNESS_TASK_ID, 
                                         HLR_LATENCY_PRIORITY, NULL,
                                         Runtime::protect_event(domain_ready));
#ifdef LEGION_SPY
        LegionSpy::log_event_dependence(domain_ready, disjointness_event);
#endif
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::create_pending_cross_product(IndexPartition handle1,
                                                        IndexPartition handle2,
                            std::map<DomainPoint,IndexPartition> &our_handles,
                            std::map<DomainPoint,IndexPartition> &user_handles,
                                                        PartitionKind kind,
                                                        ColorPoint &part_color,
                                                        bool allocable,
                                                        ApEvent handle_ready,
                                                        ApEvent domain_ready)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *base = get_node(handle1);
      IndexPartNode *source = get_node(handle2);
      // Iterate over all our sub-regions and generate partitions
      for (Domain::DomainPointIterator itr(base->color_space); itr; itr++)
      {
        ColorPoint child_color(itr.p);
        IndexSpaceNode *child_node = base->get_child(child_color); 
        ColorPoint partition_color = part_color;
        if (!partition_color.is_valid())
          partition_color = ColorPoint(DomainPoint::from_point<1>(
              LegionRuntime::Arrays::Point<1>(child_node->generate_color())));
        IndexPartition pid(runtime->get_unique_index_partition_id(),
                           handle1.get_tree_id());
        create_pending_partition(pid, child_node->handle,
                                 source->color_space, partition_color,
                                 kind, allocable, handle_ready, domain_ready);
        // Save the handles for ourselves 
        our_handles[itr.p] = pid;
        // If the user requested the handle for this point return it
        std::map<DomainPoint,IndexPartition>::iterator finder = 
          user_handles.find(itr.p);
        if (finder != user_handles.end())
          finder->second = pid;
      }
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::create_partition_by_field(RegionTreeContext ctx,
                                                  Operation *op, unsigned index,
                                                  const RegionRequirement &req,
                                                  IndexPartition pending,
                                                  const Domain &color_space,
                                                  ApEvent term_event,
                                                  VersionInfo &version_info,
                                              std::set<RtEvent> &applied_events)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == REG_PROJECTION);
      assert(req.privilege_fields.size() == 1);
#endif
      IndexPartNode *pending_node = get_node(pending);
      IndexSpaceNode *parent_node = pending_node->parent;
      RegionNode *top_node = get_node(req.region);
      FieldSpaceNode *field_space = top_node->get_column_source();
      // Get the index for the field
      FieldID field_id = *(req.privilege_fields.begin());
      // Traverse the target node and get all the field data descriptors
      std::set<ApEvent> preconditions;
      std::vector<FieldDataDescriptor> field_data;
      {
        FieldMask user_mask;
        user_mask.set_bit(field_space->get_field_index(field_id));
        RegionUsage usage(req);
        top_node->find_field_descriptors(ctx.get_id(), term_event, usage,
                                       user_mask, field_id, op, index,
                                       field_data, preconditions, 
                                       version_info, applied_events);
      }
      // Enumerate the color space so we can get back a different index
      // for each color in the color space
      std::map<DomainPoint,Realm::IndexSpace> subspaces;
      for (Domain::DomainPointIterator itr(color_space); itr; itr++)
      {
        subspaces[itr.p] = Realm::IndexSpace::NO_SPACE;
      }
      // Merge preconditions for all the field data descriptors
      ApEvent precondition = Runtime::merge_events(preconditions);
      // Ask the parent node to make all the subspaces
      ApEvent result = parent_node->create_subspaces_by_field(field_data,
                subspaces, ((pending_node->mode & MUTABLE) != 0), precondition);
#ifdef LEGION_SPY
      LegionSpy::log_event_dependence(precondition, result);
#endif
      // Now update the domains for all the sub-regions
      for (Domain::DomainPointIterator itr(color_space); itr; itr++)
      {
        IndexSpaceNode *child_node = pending_node->get_child(ColorPoint(itr.p));
        child_node->set_domain(subspaces[itr.p]);
      }
      return result;
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::create_partition_by_image(RegionTreeContext ctx,
                                                  Operation *op, unsigned index,
                                                  const RegionRequirement &req,
                                                  IndexPartition pending,
                                                  const Domain &color_space,
                                                  ApEvent term_event,
                                                  VersionInfo &version_info,
                                              std::set<RtEvent> &applied_events)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == PART_PROJECTION);
      assert(req.privilege_fields.size() == 1);
#endif
      IndexPartNode *pending_node = get_node(pending);
      IndexSpaceNode *parent_node = pending_node->parent;
      PartitionNode *projection_node = get_node(req.partition);
      FieldSpaceNode *field_space = projection_node->get_column_source();
      // Get the index for the field
      FieldID field_id = *(req.privilege_fields.begin());
      // Traverse the target node and get all the field data descriptors
      // Get all the index spaces from the color space in the projection
      std::set<ApEvent> preconditions;
      std::vector<FieldDataDescriptor> field_data;
      std::map<Realm::IndexSpace,Realm::IndexSpace> subspaces;
      for (Domain::DomainPointIterator itr(color_space); itr; itr++)
      {
        FieldMask user_mask;
        user_mask.set_bit(field_space->get_field_index(field_id));
        ColorPoint child_color(itr.p);
        // Open up the child on the partition node
        projection_node->open_physical_child(ctx.get_id(), child_color, 
                                             user_mask, version_info);
        RegionNode *child_node = projection_node->get_child(child_color);
        // Get the field data on this child node
        RegionUsage usage(req);
        child_node->find_field_descriptors(ctx.get_id(), term_event, usage,
                                           user_mask, field_id, op, index,
                                           field_data, preconditions, 
                                           version_info, applied_events);
        ApEvent child_pre;
        const Domain &child_dom = 
                        child_node->row_source->get_domain(child_pre);
        if (child_pre.exists())
          preconditions.insert(child_pre);
        subspaces[child_dom.get_index_space()] = Realm::IndexSpace::NO_SPACE;
      }
      // Merge the preconditions for all the field descriptors
      ApEvent precondition = Runtime::merge_events(preconditions);
      // Ask the parent node to make all the subspaces
      ApEvent result = parent_node->create_subspaces_by_image(field_data,
                subspaces, ((pending_node->mode & MUTABLE) != 0), precondition);
#ifdef LEGION_SPY
      LegionSpy::log_event_dependence(precondition, result);
#endif
      // Now update the domains for all the sub-regions
      for (Domain::DomainPointIterator itr(color_space); itr; itr++)
      {
        ColorPoint child_color(itr.p);
        RegionNode     *orig_child = projection_node->get_child(child_color);
        IndexSpaceNode *next_child = pending_node->get_child(child_color);
        const Domain &orig_dom = orig_child->get_domain_no_wait();
        next_child->set_domain(subspaces[orig_dom.get_index_space()]);
      }
      return result;
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::create_partition_by_preimage(
                                                  RegionTreeContext ctx,
                                                  Operation *op, unsigned index,
                                                  const RegionRequirement &req,
                                                  IndexPartition projection,
                                                  IndexPartition pending,
                                                  const Domain &color_space,
                                                  ApEvent term_event,
                                                  VersionInfo &version_info,
                                              std::set<RtEvent> &applied_events)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == REG_PROJECTION);
      assert(req.privilege_fields.size() == 1);
#endif
      IndexPartNode *pending_node = get_node(pending);
      IndexSpaceNode *parent_node = pending_node->parent;
      IndexPartNode *projection_node = get_node(projection);
      RegionNode *top_node = get_node(req.region);
      FieldSpaceNode *field_space = top_node->get_column_source();
      // Get the index for the field
      FieldID field_id = *(req.privilege_fields.begin());
      // Traverse the target node and get all the field data structures
      std::set<ApEvent> preconditions;
      std::vector<FieldDataDescriptor> field_data;
      {
        FieldMask user_mask;
        user_mask.set_bit(field_space->get_field_index(field_id));
        RegionUsage usage(req);
        top_node->find_field_descriptors(ctx.get_id(), term_event, usage,
                                         user_mask, field_id, op, index,
                                         field_data, preconditions, 
                                         version_info, applied_events);
      }
      // Get all the index spaces from the color space in the projection
      std::map<Realm::IndexSpace,Realm::IndexSpace> subspaces;
      for (Domain::DomainPointIterator itr(color_space); itr; itr++)
      {
        IndexSpaceNode *child_node = 
          projection_node->get_child(ColorPoint(itr.p));
        ApEvent child_pre;
        const Domain &child_dom = child_node->get_domain(child_pre);
        if (child_pre.exists())
          preconditions.insert(child_pre);
        subspaces[child_dom.get_index_space()] = Realm::IndexSpace::NO_SPACE;
      }
      // Merge the preconditions for all the field descriptors
      ApEvent precondition = Runtime::merge_events(preconditions);
      // Ask the parent node to make all the subspaces
      ApEvent result = parent_node->create_subspaces_by_preimage(field_data,
                subspaces, ((pending_node->mode & MUTABLE) != 0), precondition);
#ifdef LEGION_SPY
      LegionSpy::log_event_dependence(precondition, result);
#endif
      // Now update the domains for all the sub-regions
      for (Domain::DomainPointIterator itr(color_space); itr; itr++)
      {
        ColorPoint child_color(itr.p);
        IndexSpaceNode *orig_child = projection_node->get_child(child_color);
        IndexSpaceNode *next_child = pending_node->get_child(child_color);
        const Domain &orig_dom = orig_child->get_domain_no_wait();
        next_child->set_domain(subspaces[orig_dom.get_index_space()]);
      }
      return result;
    }

    //--------------------------------------------------------------------------
    IndexSpace RegionTreeForest::find_pending_space(IndexPartition parent,
                                                    const DomainPoint &color,
                                                    ApUserEvent &handle_ready,
                                                    ApUserEvent &domain_ready)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *parent_node = get_node(parent);
      ColorPoint child_color(color);
      // First get the child node   
      if (!parent_node->has_child(child_color))
      {
        log_run.error("Invalid color in compute pending space!");
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_PARTITION_COLOR);
      }
      IndexSpaceNode *child_node = parent_node->get_child(child_color);
      if (!parent_node->get_pending_child(child_color, 
                                          handle_ready, domain_ready))
      {
        log_run.error("Invalid pending child!");
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_PENDING_CHILD);
      }
      return child_node->handle;
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::compute_pending_space(IndexSpace target,
                                         const std::vector<IndexSpace> &handles,
                                                                  bool is_union)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *child_node = get_node(target);
      IndexPartNode *parent_node = child_node->parent;
      // Compute the new index space 
      std::set<ApEvent> preconditions;
      std::vector<Realm::IndexSpace> spaces(handles.size());
      unsigned idx = 0;
      for (std::vector<IndexSpace>::const_iterator it = handles.begin();
            it != handles.end(); it++, idx++)
      {
        IndexSpaceNode *node = get_node(*it); 
        ApEvent precondition;
        const Domain &dom = node->get_domain(precondition);
        spaces[idx] = dom.get_index_space();
        if (precondition.exists())
          preconditions.insert(precondition);
      }
      ApEvent parent_precondition;
      const Domain &parent_dom = 
              parent_node->parent->get_domain(parent_precondition);
      if (parent_precondition.exists())
        preconditions.insert(parent_precondition);
      // Now we can compute the low-level index space
      ApEvent precondition = Runtime::merge_events(preconditions);
      Realm::IndexSpace result;
      ApEvent ready(Realm::IndexSpace::reduce_index_spaces(
          is_union ? Realm::IndexSpace::ISO_UNION : 
                     Realm::IndexSpace::ISO_INTERSECT, spaces, result, 
          ((parent_node->mode & MUTABLE) != 0)/* allocable */,
          parent_dom.get_index_space(), precondition));
      // Now set the result and trigger the handle ready event
      child_node->set_domain(Domain(result));
#ifdef LEGION_SPY
      LegionSpy::log_event_dependence(precondition, ready);
#endif
      return ready;
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::compute_pending_space(IndexSpace target,
                                                    IndexPartition handle,
                                                    bool is_union)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *child_node = get_node(target);
      IndexPartNode *parent_node = child_node->parent;
      IndexPartNode *reduce_node = get_node(handle);
      std::set<ApEvent> preconditions;
      std::vector<Realm::IndexSpace> 
        spaces(reduce_node->color_space.get_volume());
      unsigned idx = 0;
      for (Domain::DomainPointIterator itr(reduce_node->color_space); 
            itr; itr++, idx++)
      {
        ColorPoint node_color(itr.p);
        IndexSpaceNode *node = reduce_node->get_child(node_color);
        ApEvent precondition;
        const Domain &dom = node->get_domain(precondition);
        spaces[idx] = dom.get_index_space();
        if (precondition.exists())
          preconditions.insert(precondition);
      }
      ApEvent parent_precondition;
      const Domain &parent_dom = 
            parent_node->parent->get_domain(parent_precondition);
      if (parent_precondition.exists())
        preconditions.insert(parent_precondition);
      // Now we can compute the low-level index space
      ApEvent precondition = Runtime::merge_events(preconditions);
      Realm::IndexSpace result;
      ApEvent ready(Realm::IndexSpace::reduce_index_spaces(
          is_union ? Realm::IndexSpace::ISO_UNION : 
                     Realm::IndexSpace::ISO_INTERSECT, spaces, result,
          ((parent_node->mode & MUTABLE) != 0)/* allocable */,
          parent_dom.get_index_space(), precondition));
      // Now set the result and trigger the handle ready event
      child_node->set_domain(Domain(result));
#ifdef LEGION_SPY
      LegionSpy::log_event_dependence(precondition, ready);
#endif
      return ready;
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::compute_pending_space(IndexSpace target,
                                                  IndexSpace initial,
                                         const std::vector<IndexSpace> &handles)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *child_node = get_node(target);
      IndexPartNode *parent_node = child_node->parent;
      std::set<ApEvent> preconditions;
      std::vector<Realm::IndexSpace> spaces(handles.size()+1);
      IndexSpaceNode *init_node = get_node(initial);
      ApEvent init_precondition;
      const Domain &init_dom = init_node->get_domain(init_precondition);
      spaces[0] = init_dom.get_index_space();
      if (init_precondition.exists())
        preconditions.insert(init_precondition);
      unsigned idx = 1;
      for (std::vector<IndexSpace>::const_iterator it = handles.begin();
            it != handles.end(); it++, idx++)
      {
        IndexSpaceNode *node = get_node(*it);  
        ApEvent precondition;
        const Domain &dom = node->get_domain(precondition);
        spaces[idx] = dom.get_index_space();
        if (precondition.exists())
          preconditions.insert(precondition);
      }
      ApEvent parent_precondition;
      const Domain &parent_dom = 
              parent_node->parent->get_domain(parent_precondition);
      if (parent_precondition.exists())
        preconditions.insert(parent_precondition);
      // Now we can compute the low-level index space
      ApEvent precondition = Runtime::merge_events(preconditions);
      Realm::IndexSpace result;
      ApEvent ready(Realm::IndexSpace::reduce_index_spaces(
                             Realm::IndexSpace::ISO_SUBTRACT, spaces, result,
          ((parent_node->mode & MUTABLE) != 0)/* allocable */,
          parent_dom.get_index_space(), precondition));
      // Now set the result and trigger the handle ready event
      child_node->set_domain(Domain(result));
#ifdef LEGION_SPY
      LegionSpy::log_event_dependence(precondition, ready);
#endif
      return ready;
    }

    //--------------------------------------------------------------------------
    IndexPartition RegionTreeForest::get_index_partition(IndexSpace parent,
                                                       const ColorPoint &color)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *parent_node = get_node(parent);
      IndexPartNode *child_node = parent_node->get_child(color);
      return child_node->handle;
    }

    //--------------------------------------------------------------------------
    IndexSpace RegionTreeForest::get_index_subspace(IndexPartition parent,
                                                    const ColorPoint &color)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *parent_node = get_node(parent);
      IndexSpaceNode *child_node = parent_node->get_child(color);
      return child_node->handle;
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_multiple_domains(IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(handle);
      return node->has_component_domains();
    }

    //--------------------------------------------------------------------------
    Domain RegionTreeForest::get_index_space_domain(IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(handle);
      return node->get_domain_blocking();
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::get_index_space_domains(IndexSpace handle,
                                                   std::vector<Domain> &domains)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(handle);
      node->get_domains_blocking(domains); 
    }

    //--------------------------------------------------------------------------
    Domain RegionTreeForest::get_index_partition_color_space(IndexPartition p)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *node = get_node(p);
      return node->color_space;
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::get_index_space_partition_colors(IndexSpace sp,
                                                   std::set<ColorPoint> &colors)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(sp);
      node->get_colors(colors); 
    }

    //--------------------------------------------------------------------------
    ColorPoint RegionTreeForest::get_index_space_color(IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(handle);
      return node->color;
    }

    //--------------------------------------------------------------------------
    ColorPoint RegionTreeForest::get_index_partition_color(
                                                          IndexPartition handle)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *node = get_node(handle);
      return node->color;
    }

    //--------------------------------------------------------------------------
    IndexSpace RegionTreeForest::get_parent_index_space(IndexPartition handle)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *node = get_node(handle);
      return node->parent->handle;
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_parent_index_partition(IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(handle);
      return (node->parent != NULL);
    }

    //--------------------------------------------------------------------------
    IndexPartition RegionTreeForest::get_parent_index_partition(
                                                              IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(handle);
      if (node->parent == NULL)
      {
        log_run.error("Parent index partition requested for "
                            "index space %x with no parent. Use "
                            "has_parent_index_partition to check "
                            "before requesting a parent.", handle.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_PARENT_REQUEST);
      }
      return node->parent->handle;
    }

    //--------------------------------------------------------------------------
    unsigned RegionTreeForest::get_index_space_depth(IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      return get_node(handle)->depth;
    }

    //--------------------------------------------------------------------------
    unsigned RegionTreeForest::get_index_partition_depth(IndexPartition handle)
    //--------------------------------------------------------------------------
    {
      return get_node(handle)->depth;
    }

    //--------------------------------------------------------------------------
    IndexSpaceAllocator* RegionTreeForest::get_index_space_allocator(
                                                              IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(handle);
      return node->get_allocator();
    }

    //--------------------------------------------------------------------------
    size_t RegionTreeForest::get_domain_volume(IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(handle);
      return node->get_domain_volume(true/*app query*/); 
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::is_index_partition_disjoint(IndexPartition p)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *node = get_node(p);
      return node->is_disjoint(true/*app query*/);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::is_index_partition_complete(IndexPartition p)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *node = get_node(p);
      return node->is_complete();
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::create_field_space(FieldSpace handle)
    //--------------------------------------------------------------------------
    {
      create_node(handle);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::destroy_field_space(FieldSpace handle,
                                               AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *node = get_node(handle);
      node->destroy_node(source);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::allocate_field(FieldSpace handle, size_t field_size,
                              FieldID fid, CustomSerdezID serdez_id, bool local)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *node = get_node(handle);
      if (local && node->has_field(fid))
        return true;
      RtEvent ready = node->allocate_field(fid, field_size, serdez_id);
      if (ready.exists() && !ready.has_triggered())
        ready.wait();
      return false;
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::free_field(FieldSpace handle, FieldID fid) 
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *node = get_node(handle);
      node->free_field(fid, runtime->address_space);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::allocate_fields(FieldSpace handle, 
                                           const std::vector<size_t> &sizes,
                                           const std::vector<FieldID> &fields,
                                           CustomSerdezID serdez_id)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(sizes.size() == fields.size());
#endif
      // We know that none of these field allocations are local
      FieldSpaceNode *node = get_node(handle);
      RtEvent ready = node->allocate_fields(sizes, fields, serdez_id);
      // Wait for this to exist
      if (ready.exists() && !ready.has_triggered())
        ready.wait();
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::free_fields(FieldSpace handle,
                                       const std::vector<FieldID> &to_free)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *node = get_node(handle);
      node->free_fields(to_free, runtime->address_space);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::get_all_regions(FieldSpace handle,
                                           std::set<LogicalRegion> &regions)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *node = get_node(handle);
      node->get_all_regions(regions);
    }

    //--------------------------------------------------------------------------
    size_t RegionTreeForest::get_field_size(FieldSpace handle, FieldID fid)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *node = get_node(handle);
      if (!node->has_field(fid))
      {
        log_run.error("FieldSpace %x has no field %d", handle.id, fid);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_FIELD_ID);
      }
      return node->get_field_size(fid);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::get_field_space_fields(FieldSpace handle,
                                                  std::vector<FieldID> &fields)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *node = get_node(handle);
      node->get_all_fields(fields);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::create_logical_region(LogicalRegion handle)
    //--------------------------------------------------------------------------
    {
      create_node(handle, NULL/*parent*/);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::destroy_logical_region(LogicalRegion handle,
                                                  AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      RegionNode *node = get_node(handle);
      node->destroy_node(source);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::destroy_logical_partition(LogicalPartition handle,
                                                     AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      PartitionNode *node = get_node(handle);
      node->destroy_node(source);
    }

    //--------------------------------------------------------------------------
    LogicalPartition RegionTreeForest::get_logical_partition(
                                    LogicalRegion parent, IndexPartition handle)
    //--------------------------------------------------------------------------
    {
      // No lock needed for this one
      return LogicalPartition(parent.tree_id, handle, parent.field_space);
    }

    //--------------------------------------------------------------------------
    LogicalPartition RegionTreeForest::get_logical_partition_by_color(
                                     LogicalRegion parent, const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      RegionNode *parent_node = get_node(parent);
      IndexPartNode *index_node = parent_node->row_source->get_child(c);
      LogicalPartition result(parent.tree_id, index_node->handle, 
                              parent.field_space);
      return result;
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_logical_partition_by_color(LogicalRegion parent,
                                                        const ColorPoint &color)
    //--------------------------------------------------------------------------
    {
      RegionNode *parent_node = get_node(parent);
      return parent_node->has_color(color);
    }

    //--------------------------------------------------------------------------
    LogicalPartition RegionTreeForest::get_logical_partition_by_tree(
                      IndexPartition handle, FieldSpace space, RegionTreeID tid)
    //--------------------------------------------------------------------------
    {
      // No lock needed for this one
      return LogicalPartition(tid, handle, space);
    }

    //--------------------------------------------------------------------------
    LogicalRegion RegionTreeForest::get_logical_subregion(
                                    LogicalPartition parent, IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      // No lock needed for this one
      return LogicalRegion(parent.tree_id, handle, parent.field_space);
    }
    
    //--------------------------------------------------------------------------
    LogicalRegion RegionTreeForest::get_logical_subregion_by_color(
                                  LogicalPartition parent, const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      PartitionNode *parent_node = get_node(parent);
      IndexSpaceNode *index_node = parent_node->row_source->get_child(c);
      LogicalRegion result(parent.tree_id, index_node->handle,
                           parent.field_space);
      return result;
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_logical_subregion_by_color(
                               LogicalPartition parent, const ColorPoint &color)
    //--------------------------------------------------------------------------
    {
      PartitionNode *parent_node = get_node(parent);
      return parent_node->has_color(color);
    }

    //--------------------------------------------------------------------------
    LogicalRegion RegionTreeForest::get_logical_subregion_by_tree(
                          IndexSpace handle, FieldSpace space, RegionTreeID tid)
    //--------------------------------------------------------------------------
    {
      // No lock needed for this one
      return LogicalRegion(tid, handle, space);
    }

    //--------------------------------------------------------------------------
    ColorPoint RegionTreeForest::get_logical_region_color(LogicalRegion handle)
    //--------------------------------------------------------------------------
    {
      RegionNode *node = get_node(handle);
      return node->row_source->color;
    }

    //--------------------------------------------------------------------------
    ColorPoint RegionTreeForest::get_logical_partition_color(
                                                        LogicalPartition handle)
    //--------------------------------------------------------------------------
    {
      PartitionNode *node = get_node(handle);
      return node->row_source->color;
    }

    //--------------------------------------------------------------------------
    LogicalRegion RegionTreeForest::get_parent_logical_region(
                                                        LogicalPartition handle)
    //--------------------------------------------------------------------------
    {
      PartitionNode *node = get_node(handle);
      return node->parent->handle;
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_parent_logical_partition(LogicalRegion handle)
    //--------------------------------------------------------------------------
    {
      RegionNode *node = get_node(handle);
      return (node->parent != NULL);
    }

    //--------------------------------------------------------------------------
    LogicalPartition RegionTreeForest::get_parent_logical_partition(
                                                           LogicalRegion handle)
    //--------------------------------------------------------------------------
    {
      RegionNode *node = get_node(handle);
      if (node->parent == NULL)
      {
        log_run.error("Parent logical partition requested for "
                            "logical region (%x,%x,%d) with no parent. "
                            "Use has_parent_logical_partition to check "
                            "before requesting a parent.", 
                            handle.index_space.id,
                            handle.field_space.id,
                            handle.tree_id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_PARENT_REQUEST);
      }
      return node->parent->handle;
    }

    //--------------------------------------------------------------------------
    size_t RegionTreeForest::get_domain_volume(LogicalRegion handle)
    //--------------------------------------------------------------------------
    {
      RegionNode *node = get_node(handle);
      Domain d = node->get_domain_blocking();
      if (d.get_dim() == 0)
      {
        const Realm::ElementMask &mask = 
          d.get_index_space().get_valid_mask();
        return mask.get_num_elmts();
      }
      else
        return d.get_volume();
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::perform_dependence_analysis(
                                                  Operation *op, unsigned idx,
                                                  RegionRequirement &req,
                                                  VersionInfo &version_info,
                                                  RestrictInfo &restrict_info,
                                                  RegionTreePath &path)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_LOGICAL_ANALYSIS_CALL);
      // If this is a NO_ACCESS, then we'll have no dependences so we're done
      if (IS_NO_ACCESS(req))
        return;
      SingleTask *parent_ctx = op->get_parent();
      RegionTreeContext ctx = 
        parent_ctx->find_enclosing_context(op->find_parent_index(idx));
#ifdef DEBUG_LEGION
      assert(ctx.exists());
#endif
      RegionNode *parent_node = get_node(req.parent);
      
      FieldMask user_mask = 
        parent_node->column_source->get_field_mask(req.privilege_fields);
      // Then compute the logical user
      LogicalUser user(op, idx, RegionUsage(req), user_mask); 
      version_info.set_upper_bound_node(parent_node);
      TraceInfo trace_info(op->already_traced(), op->get_trace(), idx, req); 
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, idx, op->get_logging_name(),
                                     op->get_unique_op_id(), parent_node,
                                     ctx.get_id(), true/*before*/, 
                                     false/*premap*/,
                                     false/*closing*/, true/*logical*/,
                       FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
      // Finally do the traversal, note that we don't need to hold the
      // context lock since the runtime guarantees that all dependence
      // analysis for a single context are performed in order
      parent_node->register_logical_user(ctx.get_id(), user, path, 
                                         version_info, trace_info, 
                                         (req.handle_type != SINGULAR),
                                         true/*report uninitialized*/);
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, idx, op->get_logging_name(),
                                     op->get_unique_op_id(), parent_node,
                                     ctx.get_id(), false/*before*/, 
                                     false/*premap*/,
                                     false/*closing*/, true/*logical*/,
                       FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
      // Once we are done we can clear out the list of recorded dependences
      op->clear_logical_records();
      // Do our check for restricted coherence
      if (parent_ctx->has_restrictions())
        parent_ctx->perform_restricted_analysis(req, restrict_info);
      // If we have a restriction, then record it on the region requirement
      if (restrict_info.has_restrictions())
        req.flags |= RESTRICTED_FLAG;
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::perform_reduction_close_analysis(Operation *op,
                                                  unsigned idx,
                                                  RegionRequirement &req,
                                                  VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
      SingleTask *parent_ctx = op->get_parent();
      RegionTreeContext ctx = 
        parent_ctx->find_enclosing_context(op->find_parent_index(idx));
#ifdef DEBUG_LEGION
      assert(ctx.exists());
      assert(req.privilege == REDUCE);
#endif
      RegionNode *parent_node = get_node(req.parent);

      FieldMask user_mask = 
        parent_node->column_source->get_field_mask(req.privilege_fields);
      // Then compute the logical user
      LogicalUser user(op, idx, RegionUsage(req), user_mask);
      // Make the user read-only so we catch dependences on 
      // all the outstanding reductions
      user.usage.privilege = READ_ONLY;
      version_info.set_upper_bound_node(parent_node);
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, idx, op->get_logging_name(),
                                     op->get_unique_op_id(), parent_node,
                                     ctx.get_id(), true/*before*/, 
                                     false/*premap*/,
                                     false/*closing*/, true/*logical*/,
                       FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
      parent_node->close_reduction_analysis(ctx.get_id(), user, version_info); 
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, idx, op->get_logging_name(),
                                     op->get_unique_op_id(), parent_node,
                                     ctx.get_id(), false/*before*/, 
                                     false/*premap*/,
                                     false/*closing*/, true/*logical*/,
                       FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::perform_fence_analysis(RegionTreeContext ctx,
                                                  Operation *fence,
                                                  LogicalRegion handle,
                                                  bool dominate)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_LOGICAL_FENCE_CALL);
      // Register dependences for this fence on all users in the tree
      RegionNode *top_node = get_node(handle);
      LogicalRegistrar registrar(ctx.get_id(), fence, 
                       FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), dominate);
      top_node->visit_node(&registrar);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::perform_deletion_analysis(DeletionOp *op, 
                                                    unsigned idx,
                                                    RegionRequirement &req,
                                                    VersionInfo &version_info,
                                                    RestrictInfo &restrict_info,
                                                    RegionTreePath &path)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_LOGICAL_ANALYSIS_CALL);
      SingleTask *parent_ctx = op->get_parent();
      RegionTreeContext ctx = 
        parent_ctx->find_enclosing_context(op->find_parent_index(idx));
#ifdef DEBUG_LEGION
      assert(ctx.exists());
#endif
      RegionNode *parent_node = get_node(req.parent);
      
      FieldMask user_mask = 
        parent_node->column_source->get_field_mask(req.privilege_fields);
      // Then compute the logical user
      LogicalUser user(op, idx, RegionUsage(req), user_mask);
      version_info.set_upper_bound_node(parent_node);
      TraceInfo trace_info(op->already_traced(), op->get_trace(), idx, req);
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, idx, op->get_logging_name(),
                                     op->get_unique_op_id(), parent_node,
                                     ctx.get_id(), true/*before*/, 
                                     false/*premap*/,
                                     false/*closing*/, true/*logical*/,
                       FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
      // Do the traversal
      parent_node->register_logical_deletion(ctx.get_id(), user, user_mask, 
                               path, version_info, restrict_info, trace_info);
      // Once we are done we can clear out the list of recorded dependences
      op->clear_logical_records();
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, idx, op->get_logging_name(),
                                     op->get_unique_op_id(), parent_node,
                                     ctx.get_id(), false/*before*/, 
                                     false/*premap*/,
                                     false/*closing*/, true/*logical*/,
                       FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::initialize_current_context(RegionTreeContext ctx,
                    const RegionRequirement &req, const InstanceSet &sources,
                    ApEvent term_event, SingleTask *context,unsigned init_index,
                    std::map<PhysicalManager*,InstanceView*> &top_views)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_INITIALIZE_CONTEXT_CALL);
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *top_node = get_node(req.region);
      RegionUsage usage(req);
      FieldMask user_mask = 
        top_node->column_source->get_field_mask(req.privilege_fields);
      std::vector<LogicalView*> corresponding(sources.size());
      const AddressSpaceID local_space = context->runtime->address_space;
      // Build our set of corresponding views
      if (IS_REDUCE(req))
      {
        for (unsigned idx = 0; idx < sources.size(); idx++)
        {
          const InstanceRef &src_ref = sources[idx];
          PhysicalManager *manager = src_ref.get_manager();
#ifdef DEBUG_LEGION
          assert(manager->is_reduction_manager());
#endif
          // Check to see if the view exists yet or not
          std::map<PhysicalManager*,InstanceView*>::const_iterator 
            finder = top_views.find(manager);
          if (finder == top_views.end())
          {
            ReductionView *new_view = 
              context->create_instance_top_view(manager,
                  local_space)->as_reduction_view();
            top_views[manager] = new_view;
            corresponding[idx] = new_view;
          }
          else
            corresponding[idx] = finder->second;
        }
      }
      else
      {
        for (unsigned idx = 0; idx < sources.size(); idx++)
        {
          const InstanceRef &src_ref = sources[idx];
          PhysicalManager *manager = src_ref.get_manager();
#ifdef DEBUG_LEGION
          assert(manager->is_instance_manager());
#endif
          // Check to see if the view exists yet or not
          std::map<PhysicalManager*,InstanceView*>::const_iterator 
            finder = top_views.find(manager);
          if (finder == top_views.end())
          {
            MaterializedView *new_view = 
             context->create_instance_top_view(manager, 
                 local_space)->as_materialized_view();
            top_views[manager] = new_view;
            // See if we need to get the appropriate subview
            if (top_node != manager->region_node)
              corresponding[idx] = 
                top_node->convert_reference_region(manager, context);
            else
              corresponding[idx] = new_view;
          }
          else
          {
            // See if we have to pull down the right subview
            if (top_node != manager->region_node)
              corresponding[idx] = 
                top_node->convert_reference_region(manager, context);
            else // they are the same so we can just use the view as is
              corresponding[idx] = finder->second;
          }
        }
      }
      // Now we can register all these instances
      top_node->seed_state(ctx.get_id(), term_event, usage, user_mask, 
                           sources, context, init_index, corresponding); 
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::initialize_current_context(RegionTreeContext ctx,
                                                  const RegionRequirement &req,
                                                  const InstanceSet &sources,
                                                  SingleTask *context,
                                                  unsigned init_index,
                                                  CompositeView *composite_view)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_INITIALIZE_CONTEXT_CALL);
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
      assert(sources.size() == 1);
#endif
      RegionNode *top_node = get_node(req.region);
      RegionUsage usage(req);
      FieldMask user_mask = 
        top_node->column_source->get_field_mask(req.privilege_fields);
      std::vector<LogicalView*> corresponding(1);
      corresponding[0] = composite_view;
      top_node->seed_state(ctx.get_id(), ApEvent::NO_AP_EVENT, usage,user_mask, 
                           sources, context, init_index, corresponding);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::invalidate_current_context(RegionTreeContext ctx,
                                                      LogicalRegion handle,
                                                      bool logical_users_only)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_INVALIDATE_CONTEXT_CALL);
      RegionNode *top_node = get_node(handle);
      CurrentInvalidator invalidator(ctx.get_id(), logical_users_only);
      top_node->visit_node(&invalidator);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::match_instance_fields(const RegionRequirement &req1,
                                                 const RegionRequirement &req2,
                                                 const InstanceSet &inst1,
                                                 const InstanceSet &inst2)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req1.handle_type == SINGULAR);
      assert(req2.handle_type == SINGULAR);
      assert(req1.region.field_space == req2.region.field_space);
#endif
      // We only need to check the fields shared by both region requirements
      std::set<FieldID> intersection_fields;
      if (req1.privilege_fields.size() <= req2.privilege_fields.size())
      {
        for (std::set<FieldID>::const_iterator it = 
              req1.privilege_fields.begin(); it != 
              req1.privilege_fields.end(); it++)
        {
          if (req2.privilege_fields.find(*it) != req2.privilege_fields.end())
            intersection_fields.insert(*it);
        }
      }
      else
      {
        for (std::set<FieldID>::const_iterator it = 
              req2.privilege_fields.begin(); it != 
              req2.privilege_fields.end(); it++)
        {
          if (req1.privilege_fields.find(*it) != req1.privilege_fields.end())
            intersection_fields.insert(*it);
        }
      }
      FieldSpaceNode *node = get_node(req1.region.field_space);
      FieldMask intersection_mask = node->get_field_mask(intersection_fields);
      for (unsigned idx = 0; idx < inst1.size(); idx++)
      {
        const InstanceRef &ref = inst1[idx];
        if (ref.is_composite_ref())
          continue;
        FieldMask overlap = intersection_mask & ref.get_valid_fields();
        if (!overlap)
          continue;
        PhysicalManager *manager = ref.get_manager();
        for (unsigned idx2 = 0; idx2 < inst2.size(); idx2++)
        {
          const InstanceRef &other = inst2[idx2];
          if (other.is_composite_ref())
            continue;
          // If they are not the same instance we can keep going
          if (manager != other.get_manager())
            continue;
          // There is only one of these in the set
          if (!(overlap - other.get_valid_fields()))
          {
            // Dominated all the fields, so we can remove them
            intersection_mask -= overlap;
          }
          else // We didn't dominate all the fields, so no good
              return false;
          break;
        }
        // If we've satisfied all the fields then we are done early
        if (!intersection_mask)
          break;
      }
      // If we satisfied all the fields then we are good
      return (!intersection_mask);
    }

    //--------------------------------------------------------------------------
    Restriction* RegionTreeForest::create_coherence_restriction(
                     const RegionRequirement &req, const InstanceSet &instances)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *node = get_node(req.region);  
      Restriction *result = new Restriction(node);    
      for (unsigned idx = 0; idx < instances.size(); idx++)
      {
        const InstanceRef &ref = instances[idx];
        PhysicalManager *manager = ref.get_manager();
#ifdef DEBUG_LEGION
        assert(manager->is_instance_manager());
#endif
        InstanceManager *inst = manager->as_instance_manager();
        result->add_restricted_instance(inst, ref.get_valid_fields());
      }
      return result;
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::add_acquisition(
                                    const std::list<Restriction*> &restrictions,
                                    AcquireOp *op, const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *node = get_node(req.region);
      FieldMask acquired_fields = 
        node->column_source->get_field_mask(req.privilege_fields);
      const RegionTreeID tid = req.region.get_tree_id();
      for (std::list<Restriction*>::const_iterator it = restrictions.begin();
            it != restrictions.end(); it++)
      {
        // If the tree IDs are different, we don't even bother
        if ((*it)->tree_id != tid)
          continue;
        (*it)->add_acquisition(op, node, acquired_fields);
        if (!acquired_fields)
          return true;
      }
      // Return true if don't have any more fields to acquired
      return !acquired_fields;
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::remove_acquisition(
                                    const std::list<Restriction*> &restrictions,
                                    ReleaseOp *op, const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *node = get_node(req.region);
      FieldMask released_fields = 
        node->column_source->get_field_mask(req.privilege_fields);
      const RegionTreeID tid = req.region.get_tree_id();
      for (std::list<Restriction*>::const_iterator it = restrictions.begin();
            it != restrictions.end(); it++)
      {
        // If the tree IDs are different, we don't even bother
        if ((*it)->tree_id != tid)
          continue;
        (*it)->remove_acquisition(op, node, released_fields);
        if (!released_fields)
          return true;
      }
      // Return true if we don't have any more fields to release
      return !released_fields;
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::add_restriction(
                            std::list<Restriction*> &restrictions, AttachOp *op,
                            InstanceManager *inst, const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *node = get_node(req.region);
      FieldMask restricted_fields = 
        node->column_source->get_field_mask(req.privilege_fields);
      const RegionTreeID tid = req.region.get_tree_id();
      for (std::list<Restriction*>::const_iterator it = restrictions.begin();
            it != restrictions.end(); it++)
      {
        // If the tree IDs are different, we don't even bother 
        if ((*it)->tree_id != tid)
          continue;
        (*it)->add_restriction(op, node, inst, restricted_fields);
        if (!restricted_fields)
          return;
      }
      // If we still had any restricted fields we can make a new restriction
      if (!!restricted_fields)
      {
        Restriction *restriction = new Restriction(node);
        restriction->add_restricted_instance(inst, restricted_fields);
        restrictions.push_back(restriction);
      }
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::remove_restriction(
                                     std::list<Restriction*> &restrictions, 
                                     DetachOp *op, const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *node = get_node(req.region);
      FieldMask released_fields = 
        node->column_source->get_field_mask(req.privilege_fields);
      const RegionTreeID tid = req.region.get_tree_id();
      for (std::list<Restriction*>::iterator it = restrictions.begin();
            it != restrictions.end(); /*nothing*/)
      {
        // If the tree IDs are different, we don't even bother
        if ((*it)->tree_id != tid)
        {
          it++;
          continue;
        }
        // Try ot match it first
        if ((*it)->matches(op, node, released_fields))
        {
          it = restrictions.erase(it);
          if (!released_fields)
            return true;
          continue;
        }
        // Otherwise try to remove it internally
        (*it)->remove_restriction(op, node, released_fields);
        if (!released_fields)
          return true;
        it++;
      }
      return !released_fields;
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::perform_restricted_analysis(
                      const std::list<Restriction*> &restrictions,
                      const RegionRequirement &req, RestrictInfo &restrict_info)
    //--------------------------------------------------------------------------
    {
      // Don't compute this until we need to
      RegionTreeNode *node = NULL;
      FieldMask possibly_restricted;
      const RegionTreeID tid = req.parent.get_tree_id();
      for (std::list<Restriction*>::const_iterator it = restrictions.begin();
            it != restrictions.end(); it++)
      {
        // Skip the same tree ID
        if ((*it)->tree_id != tid)
          continue;
        // If we get here we have to do the analysis, if we haven't
        // computed our node and fields yet, do that now
        if (node == NULL)
        {
          if (req.handle_type == PART_PROJECTION)
          {
            PartitionNode *part_node = get_node(req.partition);
            possibly_restricted = 
              part_node->column_source->get_field_mask(req.privilege_fields);
            node = part_node;
          }
          else
          {
            RegionNode *reg_node = get_node(req.region);
            possibly_restricted = 
              reg_node->column_source->get_field_mask(req.privilege_fields);
            node = reg_node;
          }
        }
        // Now we can do the analysis
        (*it)->find_restrictions(node, possibly_restricted, restrict_info);
        // See if we are done early
        if (!possibly_restricted)
          break;
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::physical_traverse_path(RegionTreeContext ctx,
                                                 RegionTreePath &path,
                                                 const RegionRequirement &req,
                                                 VersionInfo &version_info,
                                                 Operation *op, unsigned index,
                                                 bool find_valid,
                                                 std::set<RtEvent> &map_applied,
                                                 InstanceSet &valid_instances
#ifdef DEBUG_LEGION
                                                 , const char *log_name
                                                 , UniqueID uid
#endif
                                                  )
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_TRAVERSE_CALL);
      // If we are a NO_ACCESS, then we are already done 
      if (IS_NO_ACCESS(req))
        return;
#ifdef DEBUG_LEGION
      assert(ctx.exists());
#endif
      // Get the parent node and field mask
      RegionNode *parent_node = get_node(req.parent);
      FieldMask user_mask = 
        parent_node->column_source->get_field_mask(req.privilege_fields);
      TraversalInfo info(ctx.get_id(), op, index, req, version_info, 
                         user_mask, map_applied);
      // Build path traverser object
      PhysicalTraverser traverser(path, &info, &valid_instances);
      // Get the start node
      RegionTreeNode *start_node;
      if (req.handle_type == PART_PROJECTION)
        start_node = get_node(req.partition);
      else
        start_node = get_node(req.region);
      for (unsigned idx = 0; idx < (path.get_path_length()-1); idx++)
        start_node = start_node->get_parent();
#ifdef DEBUG_LEGION
      // Little sanity checking
      assert(start_node->get_depth() >= parent_node->get_depth());
#endif

#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, index, log_name, uid,
                                     parent_node, ctx.get_id(), 
                                     true/*before*/, true/*premap*/, 
                                     false/*closing*/, false/*logical*/,
                 FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
      traverser.traverse(start_node);
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, index, log_name, uid,
                                     parent_node, ctx.get_id(), 
                                     false/*before*/, true/*premap*/, 
                                     false/*closing*/, false/*logical*/,
                 FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::traverse_and_register(RegionTreeContext ctx,
                                                 RegionTreePath &path,
                                                 const RegionRequirement &req,
                                                 VersionInfo &version_info,
                                                 RestrictInfo &restrict_info,
                                                 Operation *op, unsigned index,
                                                 ApEvent term_event,
                                                 bool defer_add_users,
                                                 std::set<RtEvent> &map_applied,
                                                 InstanceSet &targets
#ifdef DEBUG_LEGION
                                                 , const char *log_name
                                                 , UniqueID uid
#endif
                                                 )
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, 
                        REGION_TREE_PHYSICAL_TRAVERSE_AND_REGISTER_CALL);
      // Just call physical traverse path with an empty InstanceSet
      InstanceSet empty_targets;
      physical_traverse_path(ctx, path, req, version_info, op, index, 
                             false/*find valid*/, map_applied, empty_targets
#ifdef DEBUG_LEGION
                             , log_name, uid
#endif
                             );
      // Now we can do the registration
      physical_register_only(ctx, req, version_info, restrict_info, op, index,
                             term_event, defer_add_users, map_applied, targets
#ifdef DEBUG_LEGION
                             , log_name, uid
#endif
                             );
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::map_virtual_region(RegionTreeContext ctx,
                                              const RegionRequirement &req,
                                              InstanceRef &composite_ref,
                                              VersionInfo &version_info,
                                              SingleTask *target_ctx,
                                              Operation *op,
                                              const bool needs_fields
#ifdef DEBUG_LEGION
                                              , unsigned index
                                              , const char *log_name
                                              , UniqueID uid
#endif
                                              )
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_MAP_VIRTUAL_CALL);
#ifdef DEBUG_LEGION
      assert(composite_ref.is_composite_ref() || 
              composite_ref.get_manager()->is_virtual_instance());
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *child_node = get_node(req.region);
      if (needs_fields)
      {
        FieldMask composite_mask = 
          child_node->column_source->get_field_mask(req.privilege_fields);
        CompositeView *view = child_node->map_virtual_region(ctx.get_id(),
                                composite_mask, version_info, target_ctx);
        composite_ref.set_composite_view(view, op);
      }
      else
      {
        const FieldMask &composite_mask = composite_ref.get_valid_fields();
        CompositeView *view = child_node->map_virtual_region(ctx.get_id(), 
                                 composite_mask, version_info, target_ctx);
        composite_ref.set_composite_view(view, op);
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::physical_register_only(RegionTreeContext ctx,
                                                 const RegionRequirement &req,
                                                 VersionInfo &version_info,
                                                 RestrictInfo &restrict_info,
                                                 Operation *op, unsigned index,
                                                 ApEvent term_event,
                                                 bool defer_add_users,
                                                 std::set<RtEvent> &map_applied,
                                                 InstanceSet &targets
#ifdef DEBUG_LEGION
                                                 , const char *log_name
                                                 , UniqueID uid
#endif
                                                 )
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_REGISTER_ONLY_CALL);
#ifdef DEBUG_LEGION
      assert(ctx.exists());
      assert(req.handle_type == SINGULAR);
      assert(!targets.empty());
#endif
      RegionNode *child_node = get_node(req.region);
      FieldMask user_mask = 
        child_node->column_source->get_field_mask(req.privilege_fields);
      // Construct the traversal info
      TraversalInfo info(ctx.get_id(), op, index, req, version_info, 
                         user_mask, map_applied);
      RegionUsage usage(req);
#ifdef DEBUG_LEGION 
      TreeStateLogger::capture_state(runtime, &req, index, log_name, uid,
                                     child_node, ctx.get_id(), 
                                     true/*before*/, false/*premap*/, 
                                     false/*closing*/, false/*logical*/,
                     FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
      // Perform the registration, see if we have a virtual instance or not
      if (targets[0].is_composite_ref())
      {
#ifdef DEBUG_LEGION
        assert(targets.size() == 1); // better only be one
#endif
        child_node->register_virtual(ctx.get_id(), targets[0], 
                                     version_info, user_mask);
      }
      else // this is the normal path
        child_node->register_region(info, restrict_info, term_event, 
                                    usage, defer_add_users, targets);
#ifdef DEBUG_LEGION 
      TreeStateLogger::capture_state(runtime, &req, index, log_name, uid,
                                     child_node, ctx.get_id(), 
                                     false/*before*/, false/*premap*/, 
                                     false/*closing*/, false/*logical*/,
                   FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::physical_register_users(
                                  Operation *op, ApEvent term_event,
                                  const std::vector<RegionRequirement> &regions,
                                  const std::vector<bool> &to_skip,
                                  std::vector<VersionInfo> &version_infos,
                                  std::vector<RestrictInfo> &restrict_infos,
                                 const std::vector<RegionTreeContext> &contexts,
                                  std::deque<InstanceSet> &target_sets,
                                  std::set<RtEvent> &map_applied_events)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_REGISTER_USERS_CALL);
#ifdef DEBUG_LEGION
      assert(regions.size() == to_skip.size());
      assert(regions.size() == version_infos.size());
      assert(regions.size() == target_sets.size());
#endif
      std::vector<std::vector<InstanceView*> > views(regions.size());
      // Do the precondition pass
      SingleTask *ctx = op->get_parent();
      for (unsigned idx1 = 0; idx1 < regions.size(); idx1++)
      {
        if (to_skip[idx1])
          continue;
        if (IS_NO_ACCESS(regions[idx1]) || 
            regions[idx1].privilege_fields.empty())
          continue;
        const RegionRequirement &req = regions[idx1];
        const VersionInfo &info = version_infos[idx1];
        InstanceSet &targets = target_sets[idx1];
#ifdef DEBUG_LEGION
        assert(req.handle_type == SINGULAR);
        assert(!targets.empty());
#endif
        RegionNode *region_node = get_node(req.region);
        std::vector<InstanceView*> &target_views = views[idx1];
        target_views.resize(targets.size());
        region_node->convert_target_views(targets, ctx, target_views);
        RegionUsage usage(req);
        for (unsigned idx2 = 0; idx2 < targets.size(); idx2++)
        {
          InstanceRef &ref = targets[idx2];
#ifdef DEBUG_LEGION
          assert(!ref.is_composite_ref());
#endif
          ApEvent ready = target_views[idx2]->find_user_precondition(usage, 
                            term_event, ref.get_valid_fields(), op, idx1, 
                            info, map_applied_events);
          ref.set_ready_event(ready);
        }
      }
      // Then do the registration pass
      for (unsigned idx1 = 0; idx1 < regions.size(); idx1++)
      {
        if (to_skip[idx1])
          continue;
        const RegionRequirement &req = regions[idx1];
        VersionInfo &info = version_infos[idx1];
        InstanceSet &targets = target_sets[idx1];
        std::vector<InstanceView*> &target_views = views[idx1];
        RegionUsage usage(req);
        // Information for dealing with restrictions
        FieldMask restricted_fields;
        LegionMap<LogicalView*,FieldMask>::aligned copy_out_views;
        LegionMap<ReductionView*,FieldMask>::aligned reduce_out_views;
        if (restrict_infos[idx1].has_restrictions())
          restrict_infos[idx1].populate_restrict_fields(restricted_fields);
        for (unsigned idx2 = 0; idx2 < targets.size(); idx2++)
        {
          InstanceRef &ref = targets[idx2];
          target_views[idx2]->add_user(usage, term_event, 
                                       ref.get_valid_fields(), 
                                       op, idx1, runtime->address_space,
                                       info, map_applied_events);
          if (!!restricted_fields)
          {
            FieldMask restricted = ref.get_valid_fields() & restricted_fields;
            if (!!restricted)
            {
              if (target_views[idx2]->is_reduction_view())
              {
                ReductionView *reduction_view = 
                  target_views[idx2]->as_reduction_view();
                reduce_out_views[reduction_view] = restricted;
              }
              else
                copy_out_views[target_views[idx2]] = restricted;
            }
          }
        }
        // Handle any restricted copy out operations we need to do
        if (!copy_out_views.empty() || !reduce_out_views.empty())
        {
          RegionNode *node = get_node(req.region); 
          TraversalInfo traversal_info(contexts[idx1].get_id(), op, idx1, req,
                                 info, restricted_fields, map_applied_events);
          const InstanceSet &restricted_instances = 
            restrict_infos[idx1].get_instances();
          std::vector<MaterializedView*> restricted_views(
                                             restricted_instances.size());
          node->convert_target_views(restricted_instances,ctx,restricted_views);
          if (!copy_out_views.empty())
            node->issue_restricted_copies(traversal_info, restricted_instances,
                                          restricted_views, copy_out_views);
          if (!reduce_out_views.empty())
            node->issue_restricted_reductions(traversal_info, 
                restricted_instances, restricted_views, reduce_out_views);
        }
      }
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::physical_perform_close(RegionTreeContext ctx,
                      const RegionRequirement &req, VersionInfo &version_info,
                      Operation *op, unsigned index, int composite_idx,
                      const LegionMap<ColorPoint,FieldMask>::aligned &to_close,
                      const std::set<ColorPoint> &next_children,
                      ApEvent term_event, std::set<RtEvent> &map_applied,
                      const InstanceSet &targets
#ifdef DEBUG_LEGION
                      , const char *log_name
                      , UniqueID uid
#endif
                      )
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_PERFORM_CLOSE_CALL);
      RegionNode *top_node = get_node(req.parent);
      FieldMask closing_mask = 
        top_node->column_source->get_field_mask(req.privilege_fields);
      TraversalInfo info(ctx.get_id(), op, index, req, 
                         version_info, closing_mask, map_applied);
      RegionTreeNode *close_node = (req.handle_type == PART_PROJECTION) ?
                  static_cast<RegionTreeNode*>(get_node(req.partition)) : 
                  static_cast<RegionTreeNode*>(get_node(req.region));
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, index, log_name, uid,
                                     top_node, ctx.get_id(), 
                                     true/*before*/, false/*premap*/,
                                     true/*closing*/, false/*logical*/,
                   FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), closing_mask);
#endif
      // Always build the composite instance, and then optionally issue
      // update copies to the target instances from the composite instance
      CompositeView *result = close_node->create_composite_instance(info.ctx, 
                                        to_close, next_children, closing_mask,
                                        version_info, op->get_parent());
      PhysicalState *physical_state = NULL;
      LegionMap<LogicalView*,FieldMask>::aligned valid_instances;
      bool need_valid = true;
      std::set<ApEvent> closed_events;
      RegionUsage usage(req);
      for (unsigned idx = 0; idx < targets.size(); idx++)
      {
        if (int(idx) == composite_idx)
          continue;
        // Issue the update copies to this instance
        if (need_valid)
        {
          valid_instances[result] = closing_mask;
          physical_state = close_node->get_physical_state(version_info);
          need_valid = false;
        }
        const InstanceRef &target_ref = targets[idx];
#ifdef DEBUG_LEGION
        assert(target_ref.has_ref());
#endif
        const FieldMask &target_mask = target_ref.get_valid_fields();
        InstanceView *target_view = close_node->convert_reference(target_ref, 
                                                           op->get_parent()); 
#ifdef DEBUG_LEGION
        assert(target_view->is_materialized_view());
#endif
        close_node->issue_update_copies(info, 
            target_view->as_materialized_view(), target_mask, valid_instances);
        // Update the valid views for the node
#ifdef DEBUG_LEGION
        assert(physical_state != NULL);
#endif
        close_node->update_valid_views(physical_state, target_mask,
                                       false/*dirty*/, target_view);
        // Find the user precondition
        closed_events.insert(target_view->find_user_precondition(usage,
            term_event, target_mask, op, index, version_info, map_applied));
      }
      ApEvent closed;
      if (!closed_events.empty())
        closed = Runtime::merge_events(closed_events);
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, index, log_name, uid,
                                     top_node, ctx.get_id(), 
                                     false/*before*/, false/*premap*/,
                                     true/*closing*/, false/*logical*/,
               FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), closing_mask);
#endif
      return closed;
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::physical_close_context(RegionTreeContext ctx,
                                                 const RegionRequirement &req,
                                                 VersionInfo &version_info,
                                                 Operation *op,unsigned index,
                                                 std::set<RtEvent> &map_applied,
                                                 InstanceSet &targets
#ifdef DEBUG_LEGION
                                                 , const char *log_name
                                                 , UniqueID uid
#endif
                                                 )
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_CLOSE_CONTEXT_CALL);
#ifdef DEBUG_LEGION
      assert(!targets.empty());
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *top_node = get_node(req.region);
      FieldMask user_mask = 
        top_node->column_source->get_field_mask(req.privilege_fields);
      RegionUsage usage(req);
      TraversalInfo info(ctx.get_id(), op, index, req, version_info, 
                         user_mask, map_applied);
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, index, log_name, uid,
                                     top_node, ctx.get_id(), 
                                     true/*before*/, false/*premap*/, 
                                     true/*closing*/, false/*logical*/,
                 FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
      top_node->close_state(info, usage, targets);
#ifdef DEBUG_LEGION
      TreeStateLogger::capture_state(runtime, &req, index, log_name, uid,
                                     top_node, ctx.get_id(), 
                                     false/*before*/, false/*premap*/, 
                                     true/*closing*/, false/*logical*/,
                 FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES), user_mask);
#endif
      // Look through all the targets and get their ready events
      // and then merge them together to indicate that the context is closed
      std::set<ApEvent> closed_events;
      for (unsigned idx = 0; idx < targets.size(); idx++)
        closed_events.insert(targets[idx].get_ready_event());
      if (closed_events.size() == 1)
        return *(closed_events.begin());
      return Runtime::merge_events(closed_events);
    }


    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::copy_across(RegionTreeContext src_ctx,
                                        RegionTreeContext dst_ctx,
                                        const RegionRequirement &src_req,
                                        const RegionRequirement &dst_req,
                                        const InstanceSet &src_targets, 
                                        const InstanceSet &dst_targets,
                                        VersionInfo &src_version_info, 
                                        int src_composite_index,
                                        Operation *op, unsigned index,
                                        ApEvent precondition, 
                                        std::set<RtEvent> &map_applied)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_COPY_ACROSS_CALL);
#ifdef DEBUG_LEGION
      assert(src_req.handle_type == SINGULAR);
      assert(dst_req.handle_type == SINGULAR);
      assert(src_req.instance_fields.size() == dst_req.instance_fields.size());
#endif
      std::set<ApEvent> result_events;
      std::set<ApEvent> copy_preconditions; 
      copy_preconditions.insert(precondition);
      std::vector<unsigned> src_indexes(src_req.instance_fields.size());
      std::vector<unsigned> dst_indexes(dst_req.instance_fields.size());
      // Get the field indexes for all the fields
      RegionNode *src_node = get_node(src_req.region);
      RegionNode *dst_node = get_node(dst_req.region);
      src_node->column_source->get_field_indexes(src_req.instance_fields, 
                                                 src_indexes);   
      dst_node->column_source->get_field_indexes(dst_req.instance_fields,
                                                 dst_indexes);
      // Next figure out which src_targets satisfy each field
      std::vector<unsigned> src_target_indexes(src_indexes.size());
      for (unsigned idx1 = 0; idx1 < src_targets.size(); idx1++)
      {
        const FieldMask valid_mask = src_targets[idx1].get_valid_fields();
        for (unsigned idx2 = 0; idx2 < src_indexes.size(); idx2++)
        {
          if (valid_mask.is_set(src_indexes[idx2]))
            src_target_indexes[idx2] = idx1;
        }
      }
      // Let's handle all the actual instances first
      std::vector<Domain::CopySrcDstField> src_fields;
      std::vector<Domain::CopySrcDstField> dst_fields;
      for (unsigned idx1 = 0; idx1 < dst_targets.size(); idx1++)
      {
        const InstanceRef &dst_ref = dst_targets[idx1];
#ifdef DEBUG_LEGION
        assert(!dst_ref.is_composite_ref());
        assert(dst_ref.get_manager()->is_instance_manager());
#endif
        InstanceManager *dst_manager = 
          dst_ref.get_manager()->as_instance_manager();
        FieldMask dst_valid = dst_ref.get_valid_fields();
        std::vector<unsigned> src_composite_field_indexes;
        std::vector<unsigned> dst_composite_field_indexes;
        // Iterate over all the fields and find the ones for this target
        for (unsigned idx2 = 0; idx2 < src_indexes.size(); idx2++)
        {
          if (dst_valid.is_set(dst_indexes[idx2]))
          {
            // Find the index of the source
            unsigned src_index = src_target_indexes[idx2];
            // See if this is the composite reference
            if (int(src_index) == src_composite_index)
            {
              src_composite_field_indexes.push_back(src_indexes[idx2]);
              dst_composite_field_indexes.push_back(dst_indexes[idx2]);
            }
            else
            {
              // Otherwise, this is a normal copy, fill in offsets
              const InstanceRef &src_ref = src_targets[src_index];
#ifdef DEBUG_LEGION
              assert(!src_ref.is_composite_ref());
              assert(src_ref.get_manager()->is_instance_manager());
#endif
              InstanceManager *src_manager = 
                src_ref.get_manager()->as_instance_manager();
              src_manager->compute_copy_offsets(
                  src_req.instance_fields[idx2], src_fields);
              dst_manager->compute_copy_offsets(
                  dst_req.instance_fields[idx2], dst_fields);
            }
            // Unset the bit and see if we are done with this instance
            dst_valid.unset_bit(dst_indexes[idx2]);
            if (!dst_valid)
              break;
          }
        }
        // Save the copy precondition for the dst
        copy_preconditions.insert(dst_ref.get_ready_event());
        // See if we have any composite fields to handle
        if (!src_composite_field_indexes.empty())
        {
 #ifdef DEBUG_LEGION
          assert(src_composite_index >= 0);
          assert(src_composite_field_indexes.size() == 
                 dst_composite_field_indexes.size());
#endif         
          const InstanceRef &composite_ref = src_targets[src_composite_index];
#ifdef DEBUG_LEGION
          assert(composite_ref.is_composite_ref());
#endif
          CompositeView *src_view = composite_ref.get_composite_view();
          TraversalInfo info(src_ctx.get_id(), op, index, src_req,
             src_version_info, composite_ref.get_valid_fields(), map_applied);
          InstanceView *view = 
              dst_node->convert_reference(dst_ref, op->get_parent());
#ifdef DEBUG_LEGION
          assert(view->is_materialized_view());
#endif
          MaterializedView *dst_view = view->as_materialized_view();
          ApEvent precondition = dst_ref.get_ready_event();
          // Perform the copy across
          src_view->issue_deferred_copies_across(info, dst_view, 
              src_composite_field_indexes, dst_composite_field_indexes,
              precondition, result_events);
        }
      }
      // Also record the src preconditions
#ifdef DEBUG_LEGION
      assert(src_fields.size() == dst_fields.size());
#endif
      if (!src_fields.empty())
      {
        for (unsigned idx = 0; idx < src_targets.size(); idx++)
        {
          if (int(idx) == src_composite_index)
            continue;
          copy_preconditions.insert(src_targets[idx].get_ready_event());
        }
        ApEvent copy_pre = Runtime::merge_events(copy_preconditions);
        ApEvent copy_post = dst_node->issue_copy(op, src_fields, 
                                                 dst_fields, copy_pre);
        if (copy_post.exists())
          result_events.insert(copy_post);
      }
      // Return the merge of all the result events
      return Runtime::merge_events(result_events);
    }
    
    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::reduce_across(RegionTreeContext src_ctx,
                                            RegionTreeContext dst_ctx,
                                            const RegionRequirement &src_req,
                                            const RegionRequirement &dst_req,
                                            const InstanceSet &src_targets,
                                            const InstanceSet &dst_targets,
                                            VersionInfo &src_version_info, 
                                            Operation *op, ApEvent precondition)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_REDUCE_ACROSS_CALL);
#ifdef DEBUG_LEGION
      assert(src_req.handle_type == SINGULAR);
      assert(dst_req.handle_type == SINGULAR);
      assert(src_req.instance_fields.size() == dst_req.instance_fields.size());
      assert(dst_req.privilege == REDUCE);
#endif
      std::set<ApEvent> result_events;
      std::set<ApEvent> fold_copy_preconditions, list_copy_preconditions; 
      fold_copy_preconditions.insert(precondition);
      list_copy_preconditions.insert(precondition);
      std::vector<unsigned> src_indexes(src_req.instance_fields.size());
      std::vector<unsigned> dst_indexes(dst_req.instance_fields.size());
      // Get the field indexes for all the fields
      RegionNode *src_node = get_node(src_req.region);
      RegionNode *dst_node = get_node(dst_req.region);
      src_node->column_source->get_field_indexes(src_req.instance_fields, 
                                                 src_indexes);   
      dst_node->column_source->get_field_indexes(dst_req.instance_fields,
                                                 dst_indexes);
      // Next figure out which src_targets satisfy each field
      std::vector<unsigned> src_target_indexes(src_indexes.size());
      for (unsigned idx1 = 0; idx1 < src_targets.size(); idx1++)
      {
        const FieldMask valid_mask = src_targets[idx1].get_valid_fields();
        for (unsigned idx2 = 0; idx2 < src_indexes.size(); idx2++)
        {
          if (valid_mask.is_set(src_indexes[idx2]))
            src_target_indexes[idx2] = idx1;
        }
      }
      // Let's handle all the actual instances first
      std::vector<Domain::CopySrcDstField> src_fields_fold;
      std::vector<Domain::CopySrcDstField> dst_fields_fold;
      std::vector<Domain::CopySrcDstField> src_fields_list;
      std::vector<Domain::CopySrcDstField> dst_fields_list;
      for (unsigned idx1 = 0; idx1 < dst_targets.size(); idx1++)
      {
        const InstanceRef &dst_ref = dst_targets[idx1];
#ifdef DEBUG_LEGION
        assert(!dst_ref.is_composite_ref());
#endif
        PhysicalManager *dst_manager = dst_ref.get_manager();
        FieldMask dst_valid = dst_ref.get_valid_fields();
        const bool fold = dst_manager->is_reduction_manager();
        // Iterate over all the fields and find the ones for this target
        for (unsigned idx2 = 0; idx2 < src_indexes.size(); idx2++)
        {
          if (dst_valid.is_set(dst_indexes[idx2]))
          {
            // Find the index of the source
            unsigned src_index = src_target_indexes[idx2];
            // Otherwise, this is a normal copy, fill in offsets
            const InstanceRef &src_ref = src_targets[src_index];
#ifdef DEBUG_LEGION
            assert(!src_ref.is_composite_ref());
#endif
            PhysicalManager *src_manager = src_ref.get_manager();
            if (src_manager->is_reduction_manager())
            {
              FieldMask src_mask;
              src_mask.set_bit(src_indexes[idx2]);
              src_manager->as_reduction_manager()->find_field_offsets(
                  src_mask, fold ? src_fields_fold : src_fields_list);
            }
            else
              src_manager->as_instance_manager()->compute_copy_offsets(
                  src_req.instance_fields[idx2], 
                  fold ? src_fields_fold : src_fields_list);
            if (fold)
            {
              FieldMask dst_mask;
              dst_mask.set_bit(dst_indexes[idx2]);
              dst_manager->as_reduction_manager()->find_field_offsets(
                  dst_mask, dst_fields_fold);
              fold_copy_preconditions.insert(src_ref.get_ready_event());
            }
            else
            {
              dst_manager->as_instance_manager()->compute_copy_offsets(
                  dst_req.instance_fields[idx2], dst_fields_list);
              list_copy_preconditions.insert(src_ref.get_ready_event());
            }
            // Unset the bit and see if we are done with this instance
            dst_valid.unset_bit(dst_indexes[idx2]);
            if (!dst_valid)
              break;
          }
        }
        // Save the copy precondition for the dst
        if (fold)
          fold_copy_preconditions.insert(dst_ref.get_ready_event());
        else
          list_copy_preconditions.insert(dst_ref.get_ready_event());
      }
      // See if we have any fold copies
      if (!dst_fields_fold.empty())
      {
        ApEvent copy_pre = Runtime::merge_events(fold_copy_preconditions);
        ApEvent copy_post = dst_node->issue_copy(op, 
                            src_fields_fold, dst_fields_fold, copy_pre, 
                            NULL/*intersect*/, dst_req.redop, true/*fold*/);
        if (copy_post.exists())
          result_events.insert(copy_post);
      }
      // See if we have any reduction copies
      if (!dst_fields_list.empty())
      {
        ApEvent copy_pre = Runtime::merge_events(list_copy_preconditions);
        ApEvent copy_post = dst_node->issue_copy(op, 
                            src_fields_list, dst_fields_list, copy_pre, 
                            NULL/*intersect*/, dst_req.redop, false/*fold*/);
        if (copy_post.exists())
          result_events.insert(copy_post);
      }
      return Runtime::merge_events(result_events);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::convert_views_into_context(
                                        const RegionRequirement &req,
                                        SingleTask *context,
                                        unsigned index,
                                        VersionInfo &version_info,
                                        InstanceView *src_view,
                                        InstanceView *dst_view,
                                        ApEvent ready_event,
                                        const std::vector<ColorPoint> &path,
                                        std::set<RtEvent> &applied_events)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_CONVERT_VIEWS_INTO_CALL);
      if (IS_NO_ACCESS(req))
        return;
      PhysicalManager *manager = src_view->get_manager();
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
      assert(dst_view->get_manager() == manager);
      assert(dst_view->is_owner());
      assert(src_view->is_owner());
#endif
      RegionNode *region_node = get_node(req.region);
      FieldMask user_mask = 
        region_node->column_source->get_field_mask(req.privilege_fields);
      // We can filter the user mask based on the allocated fields
      user_mask &= manager->layout->allocated_fields;
      if (!user_mask)
        return;
      // Walk down the path for both views to get the proper subviews
#ifdef DEBUG_LEGION
      assert(!path.empty());
#endif
      // Skip the first entry as it is the color for the root node
      for (int i = int(path.size())-2; i >= 0; i--)
      {
        src_view = src_view->get_instance_subview(path[i]);
        dst_view = dst_view->get_instance_subview(path[i]);
      }
#ifdef DEBUG_LEGION
      assert(!IS_REDUCE(req)); // no virtual mapped reductions
#endif
      RegionUsage usage(req);
      // Privileges here should always be read-write exclusive
      // to ensure that we are conservative about summarizing
      // all the users that we just collapsed
      usage.privilege = READ_WRITE; 
      usage.prop = EXCLUSIVE;
      // Add the user to the source view and then record the resulting
      // event as a the intial user for the destination view
      ApEvent init_event = src_view->add_user_fused(usage,ready_event,user_mask,
                                                  context, index, version_info, 
                                                  runtime->address_space,
                                                  applied_events,
                                                  false/*update versions*/);
      if (init_event.exists())
      {
        dst_view->add_initial_user(init_event, usage, user_mask,
                                   context->get_unique_op_id(), index);
#ifdef LEGION_SPY
        // We need to correlate the causality here for Legion Spy
        LegionSpy::log_event_dependence(init_event, ready_event); 
#endif
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::convert_views_from_context(
                                            const RegionRequirement &req,
                                            SingleTask *context, unsigned index,
                                            VersionInfo &version_info,
                                            InstanceView *dst_view,
                                            ApEvent ready_event, bool init,
                                            std::set<RtEvent> &applied_events)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_CONVERT_VIEWS_FROM_CALL);
      if (IS_NO_ACCESS(req))
        return;
      PhysicalManager *manager = dst_view->get_manager();
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
      assert(dst_view->is_owner());
#endif
      RegionNode *region_node = get_node(req.region);
      FieldMask user_mask = 
        region_node->column_source->get_field_mask(req.privilege_fields);
      // We can filter the user mask based on the allocated fields
      user_mask &= manager->layout->allocated_fields;
      if (!user_mask)
        return;
      RegionNode *root = manager->region_node;
      // Compute the proper subview if necessary
      if (region_node != root)
      {
        std::vector<ColorPoint> path;
        if (compute_index_path(root->handle.get_index_space(),
                               region_node->handle.get_index_space(), path))
        {
          // Skip the first entry as it is the color for the root node
          for (int i = int(path.size())-2; i >= 0; i--)
            dst_view = dst_view->get_instance_subview(path[i]);
        }
      }
      RegionUsage usage(req);
      // Privileges should always be exclusive here
      usage.prop = EXCLUSIVE;
      if (init)
        dst_view->add_initial_user(ready_event, usage, user_mask,
                                   context->get_unique_op_id(), index);
      else
        dst_view->add_user_fused(usage, ready_event, user_mask, context, index,
            version_info, runtime->address_space, applied_events);
    }

    //--------------------------------------------------------------------------
    int RegionTreeForest::physical_convert_mapping(Operation *op,
                                  const RegionRequirement &req,
                                  const std::vector<MappingInstance> &chosen,
                                  InstanceSet &result, RegionTreeID &bad_tree,
                                  std::vector<FieldID> &missing_fields,
                                  std::map<PhysicalManager*,
                                       std::pair<unsigned,bool> > *acquired,
                                  std::vector<PhysicalManager*> &unacquired,
                                  const bool do_acquire_checks)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_CONVERT_MAPPING_CALL);
#ifdef DEBUG_LEGION
      // Can be a part projection if we are closing to a partition node
      assert((req.handle_type == SINGULAR) || 
              (req.handle_type == PART_PROJECTION));
#endif
      RegionTreeNode *tree_node = (req.handle_type == SINGULAR) ? 
        static_cast<RegionTreeNode*>(get_node(req.region)) : 
        static_cast<RegionTreeNode*>(get_node(req.partition));      
      // Get the field mask for the fields we need
      FieldMask needed_fields = 
                tree_node->column_source->get_field_mask(req.privilege_fields);
      const RegionTreeID local_tree = tree_node->get_tree_id();
      // Iterate over each one of the chosen instances
      bool has_composite = false;
      for (std::vector<MappingInstance>::const_iterator it = chosen.begin();
            it != chosen.end(); it++)
      {
        PhysicalManager *manager = it->impl;
        if (manager == NULL)
          continue;
        if (manager->is_virtual_manager())
        {
          has_composite = true;
          continue;
        }
        // Check to see if the tree IDs are the same
        if (local_tree != manager->region_node->handle.get_tree_id())
        {
          bad_tree = manager->region_node->handle.get_tree_id();
          return -1;
        }
        // See if we should be checking the acquired sets
        if (do_acquire_checks && (acquired->find(manager) == acquired->end()))
          unacquired.push_back(manager);
        // See which fields need to be made valid here
        FieldMask valid_fields = 
          manager->layout->allocated_fields & needed_fields;
        if (!valid_fields)
          continue;
        result.add_instance(InstanceRef(manager, valid_fields));
        // We can remove the update fields from the needed mask since
        // we now have space for them
        needed_fields -= valid_fields;
        // If we've seen all our needed fields then we are done
        if (!needed_fields)
          break;
      }
      // If we don't have needed fields, see if we had a composite instance
      // if we did, put all the fields in there, otherwise we put report
      // them as missing fields figure out what field IDs they are
      if (!!needed_fields)
      {
        if (has_composite)
        {
          int composite_idx = result.size();
          result.add_instance(
            InstanceRef(VirtualManager::get_virtual_instance(), needed_fields));
          return composite_idx;
        }
        else
        {
          // This can be slow because if we get here we are just 
          // going to be reporting an error so performance no
          // longer matters
          std::set<FieldID> missing;
          tree_node->column_source->get_field_set(needed_fields, missing);
          missing_fields.insert(missing_fields.end(), 
                                missing.begin(), missing.end());
        }
      }
      // We'll only run this code when we're checking for errors
      if (!unacquired.empty())
      {
#ifdef DEBUG_LEGION
        assert(acquired != NULL);
#endif
        perform_missing_acquires(op, *acquired, unacquired); 
      }
      return -1; // no composite index
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::physical_convert_postmapping(Operation *op,
                                     const RegionRequirement &req,
                                     const std::vector<MappingInstance> &chosen,
                                     InstanceSet &result,RegionTreeID &bad_tree,
                                     std::map<PhysicalManager*,
                                          std::pair<unsigned,bool> > *acquired,
                                     std::vector<PhysicalManager*> &unacquired,
                                     const bool do_acquire_checks)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *reg_node = get_node(req.region);      
      // Get the field mask for the fields we need
      FieldMask optional_fields = 
                reg_node->column_source->get_field_mask(req.privilege_fields);
      const RegionTreeID local_tree = reg_node->handle.get_tree_id();
      // Iterate over each one of the chosen instances
      bool has_composite = false;
      for (std::vector<MappingInstance>::const_iterator it = chosen.begin();
            it != chosen.end(); it++)
      {
        PhysicalManager *manager = it->impl;
        if (manager == NULL)
          continue;
        if (manager->is_virtual_manager())
        {
          has_composite = true;
          continue;
        }
        // Check to see if the tree IDs are the same
        if (local_tree != manager->region_node->handle.get_tree_id())
        {
          bad_tree = manager->region_node->handle.get_tree_id();
          return -1;
        }
        // See if we should be checking the acquired sets
        if (do_acquire_checks && (acquired->find(manager) == acquired->end()))
          unacquired.push_back(manager);
        FieldMask valid_fields = 
          manager->layout->allocated_fields & optional_fields;
        if (!valid_fields)
          continue;
        result.add_instance(InstanceRef(manager, valid_fields));
      }
      if (!unacquired.empty())
      {
#ifdef DEBUG_LEGION
        assert(acquired != NULL);
#endif
        perform_missing_acquires(op, *acquired, unacquired);
      }
      return has_composite;
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::log_mapping_decision(UniqueID uid, unsigned index,
                                                const RegionRequirement &req,
                                                const InstanceSet &targets,
                                                bool postmapping)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(Runtime::legion_spy_enabled); 
      assert((req.handle_type == SINGULAR) || 
          (req.handle_type == PART_PROJECTION));
#endif
      FieldSpaceNode *node = (req.handle_type == SINGULAR) ? 
        get_node(req.region.get_field_space()) : 
        get_node(req.partition.get_field_space());
      for (unsigned idx = 0; idx < targets.size(); idx++)
      {
        const InstanceRef &inst = targets[idx];
        const FieldMask &valid_mask = inst.get_valid_fields();
        PhysicalManager *manager = inst.get_manager();
        std::vector<FieldID> valid_fields;
        node->get_field_ids(valid_mask, valid_fields);
        if (manager->is_virtual_manager())
        {
#ifdef DEBUG_LEGION
          assert(!postmapping);
#endif
          for (std::vector<FieldID>::const_iterator it = valid_fields.begin();
                it != valid_fields.end(); it++)
            LegionSpy::log_mapping_decision(uid, index, *it, 0/*iid*/);
        }
        else
        {
          for (std::vector<FieldID>::const_iterator it = valid_fields.begin();
                it != valid_fields.end(); it++)
          {
            if (postmapping)
              LegionSpy::log_post_mapping_decision(uid, index, *it,
                                                   manager->instance.id);
            else
              LegionSpy::log_mapping_decision(uid, index, *it,
                                              manager->instance.id);
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::perform_missing_acquires(Operation *op,
                std::map<PhysicalManager*,std::pair<unsigned,bool> > &acquired,
                                const std::vector<PhysicalManager*> &unacquired)
    //--------------------------------------------------------------------------
    {
      // This code is very similar to what we see in the memory managers
      std::map<MemoryManager*,MapperManager::AcquireStatus> remote_acquires;
      // Try and do the acquires for any instances that weren't acquired
      for (std::vector<PhysicalManager*>::const_iterator it = 
            unacquired.begin(); it != unacquired.end(); it++)
      {
        if ((*it)->try_add_base_valid_ref(MAPPING_ACQUIRE_REF, op,
                                            !(*it)->is_owner()))
        {
          acquired.insert(std::pair<PhysicalManager*,
             std::pair<unsigned,bool> >(*it,std::pair<unsigned,bool>(1,false)));
          continue;
        }
        // If we failed on the ownr node, it will never work
        // otherwise, we want to try to do a remote acquire
        else if ((*it)->is_owner())
          continue;
        remote_acquires[(*it)->memory_manager].instances.insert(*it);
      }
      if (!remote_acquires.empty())
      {
        std::set<RtEvent> done_events;
        for (std::map<MemoryManager*,MapperManager::AcquireStatus>::iterator
              it = remote_acquires.begin(); it != remote_acquires.end(); it++)
        {
          RtEvent wait_on = it->first->acquire_instances(it->second.instances,
                                                         it->second.results);
          if (wait_on.exists())
            done_events.insert(wait_on);
        }
        if (!done_events.empty())
        {
          RtEvent ready = Runtime::merge_events(done_events);
          ready.wait();
        }
        // Now figure out which ones we successfully acquired and which 
        // ones failed to be acquired
        for (std::map<MemoryManager*,MapperManager::AcquireStatus>::iterator
              req_it = remote_acquires.begin(); 
              req_it != remote_acquires.end(); req_it++)
        {
          unsigned idx = 0;
          for (std::set<PhysicalManager*>::const_iterator it = 
                req_it->second.instances.begin(); it !=
                req_it->second.instances.end(); it++, idx++)
          {
            if (req_it->second.results[idx])
            {
              acquired.insert(std::pair<PhysicalManager*,std::pair<unsigned,
                  bool> >(*it,std::pair<unsigned,bool>(1,false)));
              // make the reference a local reference and 
              // remove our remote did reference
              (*it)->add_base_valid_ref(MAPPING_ACQUIRE_REF, op);
              (*it)->send_remote_valid_update(req_it->first->owner_space,
                                          NULL, 1, false/*add*/);
            }
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::are_colocated(
                            const std::vector<InstanceSet*> &instances,
                            FieldSpace handle, const std::set<FieldID> &fields,
                            unsigned &bad1, unsigned &bad2)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *node = get_node(handle);
      const FieldMask coloc_mask = node->get_field_mask(fields);
      std::map<PhysicalManager*,FieldMask> colocate_instances;
      // Figure out the first set
      InstanceSet &first_set = *(instances[0]);
      for (unsigned idx = 0; idx < first_set.size(); idx++)
      {
        FieldMask overlap = coloc_mask & first_set[idx].get_valid_fields();
        if (!overlap)
          continue;
        PhysicalManager *manager = first_set[idx].get_manager();
        // Not allowed to have virtual views here
        if (manager->is_virtual_manager())
        {
          bad1 = 0;
          bad2 = 0;
          return false;
        }
        colocate_instances[manager] = overlap;
      }
      // Now we've got the first set, check all the rest
      for (unsigned idx1 = 0; idx1 < instances.size(); idx1++)
      {
        InstanceSet &next_set = *(instances[idx1]);
        for (unsigned idx2 = 0; idx2 < next_set.size(); idx2++)
        {
          FieldMask overlap = coloc_mask & next_set[idx2].get_valid_fields();
          if (!overlap)
            continue;
          PhysicalManager *manager = next_set[idx2].get_manager();
          if (manager->is_virtual_manager())
          {
            bad1 = idx2;
            bad2 = idx2;
            return false;
          }
          std::map<PhysicalManager*,FieldMask>::const_iterator finder = 
            colocate_instances.find(manager);
          if ((finder == colocate_instances.end()) ||
              (!!(overlap - finder->second)))
          {
            bad1 = 0;
            bad2 = idx2;
            return false;
          }
        }
      }
      return true;
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::fill_fields(RegionTreeContext ctx, Operation *op,
                                          const RegionRequirement &req,
                                          const unsigned index,
                                          const void *value, size_t value_size,
                                          VersionInfo &version_info,
                                          RestrictInfo &restrict_info,
                                          InstanceSet &instances, 
                                          ApEvent precondition,
                                          std::set<RtEvent> &map_applied_events)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_FILL_FIELDS_CALL);
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *fill_node = get_node(req.region);
      FieldMask fill_mask = 
        fill_node->column_source->get_field_mask(req.privilege_fields);
      // Check to see if we have any restricted fields that
      // we need to fill eagerly
      if (restrict_info.has_restrictions())
      {
#ifdef DEBUG_LEGION
        assert(!instances.empty());
#endif
        // If we have restrictions, we have to eagerly fill these fields 
        FieldMask eager_fields;
        restrict_info.populate_restrict_fields(eager_fields);
        ApEvent done_event = fill_node->eager_fill_fields(ctx.get_id(), op, 
                           index, eager_fields, value, value_size, 
                           version_info, instances, precondition,
                           map_applied_events);
        // Remove these fields from the fill set
        fill_mask -= eager_fields;
        // If we still have fields to fill, do that now
        if (!!fill_mask)
          fill_node->fill_fields(ctx.get_id(), fill_mask,
                                 value, value_size, version_info);
        // We know the sync precondition is chained off at least
        // one eager fill so we can return the done event
        return done_event;
      }
      else
      {
#ifdef DEBUG_LEGION
        assert(instances.empty());
#endif
        // Fill in these fields on this node
        fill_node->fill_fields(ctx.get_id(), fill_mask, 
                               value, value_size, version_info); 
        // We didn't actually use the precondition so just return it
        return precondition;
      }
    }

    //--------------------------------------------------------------------------
    InstanceManager* RegionTreeForest::create_file_instance(AttachOp *attach_op,
                                                   const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *attach_node = get_node(req.region);
      return attach_node->column_source->create_file_instance(
          req.privilege_fields, attach_node, attach_op);
    }

    //--------------------------------------------------------------------------
    InstanceRef RegionTreeForest::attach_file(RegionTreeContext ctx,
                                              SingleTask *parent_ctx,
                                              const RegionRequirement &req,
                                              InstanceManager *file_instance,
                                              VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_ATTACH_FILE_CALL);
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *attach_node = get_node(req.region);
      // Perform the attachment
      return attach_node->attach_file(ctx.get_id(), parent_ctx, 
                                      file_instance->layout->allocated_fields, 
                                      req, file_instance, version_info);
    }

    //--------------------------------------------------------------------------
    ApEvent RegionTreeForest::detach_file(RegionTreeContext ctx,
                                          const RegionRequirement &req,
                                          DetachOp *detach_op,
                                          VersionInfo &version_info,
                                          const InstanceRef &ref)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REGION_TREE_PHYSICAL_DETACH_FILE_CALL);
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *detach_node = get_node(req.region);
      // Perform the detachment
      return detach_node->detach_file(ctx.get_id(), detach_op,
                                      version_info, ref);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::send_back_logical_state(RegionTreeContext local_ctx,
                                                   RegionTreeContext remote_ctx,
                                                   const RegionRequirement &req,
                                                   AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(req.handle_type == SINGULAR);
#endif
      RegionNode *top_node = get_node(req.region);  
      FieldMask send_mask = 
        top_node->column_source->get_field_mask(req.privilege_fields);
      top_node->send_back_logical_state(local_ctx.get_id(), remote_ctx.get_id(),
                                        send_mask, target);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::check_context_state(RegionTreeContext ctx)
    //--------------------------------------------------------------------------
    {
      std::map<RegionTreeID,RegionNode*> trees;
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
        trees = tree_nodes;
      }
      CurrentInitializer init(ctx.get_id());
      for (std::map<RegionTreeID,RegionNode*>::const_iterator it = 
            trees.begin(); it != trees.end(); it++)
      {
        it->second->visit_node(&init);
      }
    }

    //--------------------------------------------------------------------------
    IndexSpaceNode* RegionTreeForest::create_node(IndexSpace sp,const Domain &d,
                                                  IndexPartNode *parent,
                                                  ColorPoint color, 
                                                  IndexSpaceKind kind,
                                                  AllocateMode mode)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *result = new IndexSpaceNode(sp, d, parent, color, 
                                                  kind, mode, this);
#ifdef DEBUG_LEGION
      assert(result != NULL);
#endif
      // Check to see if someone else has already made it
      {
        // Hold the lookup lock while modifying the lookup table
        AutoLock l_lock(lookup_lock);
        std::map<IndexSpace,IndexSpaceNode*>::const_iterator it =
          index_nodes.find(sp);
        if (it != index_nodes.end())
        {
          delete result;
          return it->second;
        }
        index_nodes[sp] = result;
        index_space_requests.erase(sp);
      }
      if (parent != NULL)
        parent->add_child(result);
      
      return result;
    }

    //--------------------------------------------------------------------------
    IndexSpaceNode* RegionTreeForest::create_node(IndexSpace sp,const Domain &d,
                                                  ApEvent ready_event, 
                                                  IndexPartNode *parent,
                                                  ColorPoint color, 
                                                  IndexSpaceKind kind,
                                                  AllocateMode mode)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *result = new IndexSpaceNode(sp, d, ready_event, parent, 
                                                  color, kind, mode, this);
#ifdef DEBUG_LEGION
      assert(result != NULL);
#endif
      // Check to see if someone else has already made it
      {
        // Hold the lookup lock while modifying the lookup table
        AutoLock l_lock(lookup_lock);
        std::map<IndexSpace,IndexSpaceNode*>::const_iterator it =
          index_nodes.find(sp);
        if (it != index_nodes.end())
        {
          delete result;
          return it->second;
        }
        index_nodes[sp] = result;
        index_space_requests.erase(sp);
      }
      if (parent != NULL)
        parent->add_child(result);
      
      return result;
    }

    //--------------------------------------------------------------------------
    IndexSpaceNode* RegionTreeForest::create_node(IndexSpace sp, 
                                                  ApEvent handle_ready,
                                                  ApEvent domain_ready,
                                                  IndexPartNode *parent,
                                                  ColorPoint color,
                                                  IndexSpaceKind kind,
                                                  AllocateMode mode)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *result = new IndexSpaceNode(sp, handle_ready, 
                                                  domain_ready, parent, 
                                                  color, kind, mode, this);
#ifdef DEBUG_LEGION
      assert(result != NULL);
#endif
      // Check to see if someone else has already made it
      {
        // Hold the lookup lock while modifying the lookup table
        AutoLock l_lock(lookup_lock);
        std::map<IndexSpace,IndexSpaceNode*>::const_iterator it =
          index_nodes.find(sp);
        if (it != index_nodes.end())
        {
          delete result;
          return it->second;
        }
        index_nodes[sp] = result;
        index_space_requests.erase(sp);
      }
      if (parent != NULL)
        parent->add_child(result);
      
      return result;
    }

    //--------------------------------------------------------------------------
    IndexPartNode* RegionTreeForest::create_node(IndexPartition p, 
                                                 IndexSpaceNode *parent,
                                                 ColorPoint color, 
                                                 Domain color_space,
                                                 bool disjoint,
                                                 AllocateMode mode)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *result = new IndexPartNode(p, parent, color, color_space,
                                                disjoint, mode, this);
#ifdef DEBUG_LEGION
      assert(parent != NULL);
      assert(result != NULL);
#endif
      // Check to see if someone else has already made it
      {
        // Hold the lookup lock while modifying the lookup table
        AutoLock l_lock(lookup_lock);
        std::map<IndexPartition,IndexPartNode*>::const_iterator it =
          index_parts.find(p);
        if (it != index_parts.end())
        {
          delete result;
          return it->second;
        }
        index_parts[p] = result;
        index_part_requests.erase(p);
      }
      if (parent != NULL)
        parent->add_child(result);
      
      return result;
    }

    //--------------------------------------------------------------------------
    IndexPartNode* RegionTreeForest::create_node(IndexPartition p, 
                                                 IndexSpaceNode *parent,
                                                 ColorPoint color, 
                                                 Domain color_space,
                                                 RtEvent ready_event,
                                                 AllocateMode mode)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *result = new IndexPartNode(p, parent, color, color_space,
                                                ready_event, mode, this);
#ifdef DEBUG_LEGION
      assert(parent != NULL);
      assert(result != NULL);
#endif
      // Check to see if someone else has already made it
      {
        // Hold the lookup lock while modifying the lookup table
        AutoLock l_lock(lookup_lock);
        std::map<IndexPartition,IndexPartNode*>::const_iterator it =
          index_parts.find(p);
        if (it != index_parts.end())
        {
          delete result;
          return it->second;
        }
        index_parts[p] = result;
        index_part_requests.erase(p);
      }
      if (parent != NULL)
        parent->add_child(result);
      
      return result;
    }
 
    //--------------------------------------------------------------------------
    FieldSpaceNode* RegionTreeForest::create_node(FieldSpace space)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *result = new FieldSpaceNode(space, this);
#ifdef DEBUG_LEGION
      assert(result != NULL);
#endif
      // Hold the lookup lock while modifying the lookup table
      AutoLock l_lock(lookup_lock);
      std::map<FieldSpace,FieldSpaceNode*>::const_iterator it =
        field_nodes.find(space);
      if (it != field_nodes.end())
      {
        delete result;
        return it->second;
      }
      field_nodes[space] = result;
      field_space_requests.erase(space);
      return result;
    }

    //--------------------------------------------------------------------------
    FieldSpaceNode* RegionTreeForest::create_node(FieldSpace space,
                                                  Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *result = new FieldSpaceNode(space, this, derez);
#ifdef DEBUG_LEGION
      assert(result != NULL);
#endif
      // Hold the lookup lock while modifying the lookup table
      AutoLock l_lock(lookup_lock);
      std::map<FieldSpace,FieldSpaceNode*>::const_iterator it =
        field_nodes.find(space);
      if (it != field_nodes.end())
      {
        delete result;
        return it->second;
      }
      field_nodes[space] = result;
      field_space_requests.erase(space);
      return result;
    }

    //--------------------------------------------------------------------------
    RegionNode* RegionTreeForest::create_node(LogicalRegion r, 
                                              PartitionNode *parent)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      if (parent != NULL)
      {
        assert(r.field_space == parent->handle.field_space);
        assert(r.tree_id == parent->handle.tree_id);
      }
#endif
      IndexSpaceNode *row_src = get_node(r.index_space);
      FieldSpaceNode *col_src = get_node(r.field_space);
      RegionNode *result = new RegionNode(r, parent, row_src, 
                                          col_src, this);
#ifdef DEBUG_LEGION
      assert(result != NULL);
#endif
      // Special case here in case multiple clients attempt to
      // make the node at the same time
      {
        // Hold the lookup lock when modifying the lookup table
        AutoLock l_lock(lookup_lock);
        // Check to see if it already exists
        std::map<LogicalRegion,RegionNode*>::const_iterator it =
          region_nodes.find(r);
        if (it != region_nodes.end())
        {
          // It already exists, delete our copy and return
          // the one that has already been made
          delete result;
          return it->second;
        }
        // Now we can add it to the map
        region_nodes[r] = result;
        // If this is a top level region add it to the collection
        // of top level tree IDs
        if (parent == NULL)
        {
#ifdef DEBUG_LEGION
          assert(tree_nodes.find(r.tree_id) == tree_nodes.end());
#endif
          tree_nodes[r.tree_id] = result;
          region_tree_requests.erase(r.tree_id);
        }
      }
      // Now we can make the other ways of accessing the node available
      if (parent == NULL)
        col_src->add_instance(result);
      row_src->add_instance(result);
      if (parent != NULL)
        parent->add_child(result);
      
      return result;
    }

    //--------------------------------------------------------------------------
    PartitionNode* RegionTreeForest::create_node(LogicalPartition p,
                                                 RegionNode *parent)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(parent != NULL);
      assert(p.field_space == parent->handle.field_space);
      assert(p.tree_id = parent->handle.tree_id);
#endif
      IndexPartNode *row_src = get_node(p.index_partition);
      FieldSpaceNode *col_src = get_node(p.field_space);
      PartitionNode *result = new PartitionNode(p, parent, row_src, 
                                                col_src, this);
#ifdef DEBUG_LEGION
      assert(result != NULL);
#endif
      // Special case here in case multiple clients attempt
      // to make the node at the same time
      {
        // Hole the lookup lock when modifying the lookup table
        AutoLock l_lock(lookup_lock);
        std::map<LogicalPartition,PartitionNode*>::const_iterator it =
          part_nodes.find(p);
        if (it != part_nodes.end())
        {
          // It already exists, delete our copy and
          // return the one that has already been made
          delete result;
          return it->second;
        }
        // Now we can put the node in the map
        part_nodes[p] = result;
      }
      // Now we can make the other ways of accessing the node available
      row_src->add_instance(result);
      parent->add_child(result);
      
      return result;
    }

    //--------------------------------------------------------------------------
    IndexSpaceNode* RegionTreeForest::get_node(IndexSpace space)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/); 
        std::map<IndexSpace,IndexSpaceNode*>::const_iterator finder = 
          index_nodes.find(space);
        if (finder != index_nodes.end())
          return finder->second;
      }
      // Couldn't find it, so send a request to the owner node
      AddressSpace owner = IndexSpaceNode::get_owner_space(space, runtime);
      if (owner == runtime->address_space)
      {
        log_index.error("Unable to find entry for index space %x.", space.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_INDEX_SPACE_ENTRY);
      }
      // Retake the lock and get something to wait on
      RtEvent wait_on;
      {
        AutoLock l_lock(lookup_lock);
        // Check to make sure we didn't loose the race
        std::map<IndexSpace,IndexSpaceNode*>::const_iterator finder = 
          index_nodes.find(space);
        if (finder != index_nodes.end())
          return finder->second;
        // Still doesn't exists, see if we sent a request already
        std::map<IndexSpace,RtEvent>::const_iterator wait_finder = 
          index_space_requests.find(space);
        if (wait_finder == index_space_requests.end())
        {
          RtUserEvent done = Runtime::create_rt_user_event();
          index_space_requests[space] = done;
          Serializer rez;
          rez.serialize(space);
          rez.serialize(done);
          runtime->send_index_space_request(owner, rez);     
          wait_on = done;
        }
        else
          wait_on = wait_finder->second;
      }
      // Wait on the event
      wait_on.wait();
      AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
      std::map<IndexSpace,IndexSpaceNode*>::const_iterator finder = 
          index_nodes.find(space);
      if (finder == index_nodes.end())
      {
        log_index.error("Unable to find entry for index space %x."
                        "This is definitely a runtime bug.", space.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_INDEX_SPACE_ENTRY);
      }
      return finder->second;
    }

    //--------------------------------------------------------------------------
    IndexPartNode* RegionTreeForest::get_node(IndexPartition part)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
        std::map<IndexPartition,IndexPartNode*>::const_iterator finder =
          index_parts.find(part);
        if (finder != index_parts.end())
          return finder->second;
      }
      // Couldn't find it, so send a request to the owner node
      AddressSpace owner = IndexPartNode::get_owner_space(part, runtime);
      if (owner == runtime->address_space)
      {
        log_index.error("Unable to find entry for index partition %x.",part.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_INDEX_PART_ENTRY);
      }
      RtEvent wait_on;
      {
        // Retake the lock in exclusive mode and make
        // sure we didn't loose the race
        AutoLock l_lock(lookup_lock);
        std::map<IndexPartition,IndexPartNode*>::const_iterator finder =
          index_parts.find(part);
        if (finder != index_parts.end())
          return finder->second;
        // See if we've already sent the request or not
        std::map<IndexPartition,RtEvent>::const_iterator wait_finder = 
          index_part_requests.find(part);
        if (wait_finder == index_part_requests.end())
        {
          RtUserEvent done = Runtime::create_rt_user_event();
          index_part_requests[part] = done;
          Serializer rez;
          rez.serialize(part);
          rez.serialize(done);
          runtime->send_index_partition_request(owner, rez);    
          wait_on = done;
        }
        else
          wait_on = wait_finder->second;
      }
      // Wait for the event
      wait_on.wait();
      AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
      std::map<IndexPartition,IndexPartNode*>::const_iterator finder = 
        index_parts.find(part);
      if (finder == index_parts.end())
      {
        log_index.error("Unable to find entry for index partition %x. "
                        "This is definitely a runtime bug.", part.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_INDEX_PART_ENTRY);
      }
      return finder->second;
    }

    //--------------------------------------------------------------------------
    FieldSpaceNode* RegionTreeForest::get_node(FieldSpace space) 
    //--------------------------------------------------------------------------
    {
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
        std::map<FieldSpace,FieldSpaceNode*>::const_iterator finder = 
          field_nodes.find(space);
        if (finder != field_nodes.end())
          return finder->second;
      }
      // Couldn't find it, so send a request to the owner node
      AddressSpaceID owner = FieldSpaceNode::get_owner_space(space, runtime); 
      if (owner == runtime->address_space)
      {
        log_field.error("Unable to find entry for field space %x.", space.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_FIELD_SPACE_ENTRY);
      }
      RtEvent wait_on;
      {
        // Retake the lock in exclusive mode and 
        // check to make sure we didn't loose the race
        AutoLock l_lock(lookup_lock);
        std::map<FieldSpace,FieldSpaceNode*>::const_iterator finder = 
          field_nodes.find(space);
        if (finder != field_nodes.end())
          return finder->second;
        // Now see if we've already sent a request
        std::map<FieldSpace,RtEvent>::const_iterator wait_finder = 
          field_space_requests.find(space);
        if (wait_finder == field_space_requests.end())
        {
          RtUserEvent done = Runtime::create_rt_user_event();
          field_space_requests[space] = done;
          Serializer rez;
          rez.serialize(space);
          rez.serialize(done);
          runtime->send_field_space_request(owner, rez);    
          wait_on = done;
        }
        else
          wait_on = wait_finder->second;
      }
      // Wait for the event to be ready
      wait_on.wait();
      AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
      std::map<FieldSpace,FieldSpaceNode*>::const_iterator finder = 
        field_nodes.find(space);
      if (finder == field_nodes.end())
      {
        log_field.error("Unable to find entry for field space %x. "
                        "This is definitely a runtime bug.", space.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_FIELD_SPACE_ENTRY);
      }
      return finder->second;
    }

    //--------------------------------------------------------------------------
    RegionNode* RegionTreeForest::get_node(LogicalRegion handle,
                                           bool need_check /* = true*/)
    //--------------------------------------------------------------------------
    {
      // Check to see if the node already exists
      bool has_top_level_region;
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
        std::map<LogicalRegion,RegionNode*>::const_iterator it = 
          region_nodes.find(handle);
        if (it != region_nodes.end())
          return it->second;
        // Check to see if we have the top level region
        if (need_check)
          has_top_level_region = 
            (tree_nodes.find(handle.get_tree_id()) != tree_nodes.end());
        else
          has_top_level_region = true;
      }
      // If we don't have the top-level region, we need to request it before
      // we go crawling up the tree so we know where to stop
      if (!has_top_level_region)
      {
        AddressSpaceID owner = 
          RegionTreeNode::get_owner_space(handle.get_tree_id(), runtime);
        if (owner == runtime->address_space)
        {
          log_region.error("Unable to find entry for logical region tree %d.",
                           handle.get_tree_id());
          assert(false);
        }
        RtEvent wait_on;
        {
          // Retake the lock and make sure we didn't loose the race
          AutoLock l_lock(lookup_lock);
          if (tree_nodes.find(handle.get_tree_id()) == tree_nodes.end())
          {
            // Still don't have it, see if we need to request it
            std::map<RegionTreeID,RtEvent>::const_iterator finder = 
              region_tree_requests.find(handle.get_tree_id());
            if (finder == region_tree_requests.end())
            {
              RtUserEvent done = Runtime::create_rt_user_event();
              region_tree_requests[handle.get_tree_id()] = done;
              Serializer rez;
              rez.serialize(handle.get_tree_id());
              rez.serialize(done);
              runtime->send_top_level_region_request(owner, rez);
              wait_on = done;
            }
            else
              wait_on = finder->second;
          }
          else
          {
            // We lost the race and it may be here now
            std::map<LogicalRegion,RegionNode*>::const_iterator it = 
              region_nodes.find(handle);
            if (it != region_nodes.end())
              return it->second;
          }
        }
        // If we did find something to wait on, do that now
        if (wait_on.exists())
        {
          if (!wait_on.has_triggered())
            wait_on.wait();
          // Retake the lock and see again if the handle we
          // were looking for was the top-level node or not
          AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
          std::map<LogicalRegion,RegionNode*>::const_iterator it = 
            region_nodes.find(handle);
          if (it != region_nodes.end())
            return it->second;
        }
      }
      // Otherwise it hasn't been made yet, so make it
      IndexSpaceNode *index_node = get_node(handle.index_space);
      if (index_node->parent != NULL)
      {
#ifdef DEBUG_LEGION
        assert(index_node->parent != NULL);
#endif
        LogicalPartition parent_handle(handle.tree_id, 
                              index_node->parent->handle, handle.field_space);
        // Note this request can recursively build more nodes, but we
        // are guaranteed that the top level node exists
        PartitionNode *parent = get_node(parent_handle, false/*need check*/);
        // Now make our node and then return it
        return create_node(handle, parent);
      }
      return create_node(handle, NULL);
    }

    //--------------------------------------------------------------------------
    PartitionNode* RegionTreeForest::get_node(LogicalPartition handle,
                                              bool need_check /* = true*/)
    //--------------------------------------------------------------------------
    {
      // Check to see if the node already exists
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
        std::map<LogicalPartition,PartitionNode*>::const_iterator it =
          part_nodes.find(handle);
        if (it != part_nodes.end())
          return it->second;
      }
      // Otherwise it hasn't been made yet so make it
      IndexPartNode *index_node = get_node(handle.index_partition);
#ifdef DEBUG_LEGION
      assert(index_node->parent != NULL);
#endif
      LogicalRegion parent_handle(handle.tree_id, index_node->parent->handle,
                                  handle.field_space);
      // Note this request can recursively build more nodes, but we
      // are guaranteed that the top level node exists
      RegionNode *parent = get_node(parent_handle, need_check);
      // Now create our node and return it
      return create_node(handle, parent);
    }

    //--------------------------------------------------------------------------
    RegionNode* RegionTreeForest::get_tree(RegionTreeID tid)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
        std::map<RegionTreeID,RegionNode*>::const_iterator finder = 
          tree_nodes.find(tid);
        if (finder != tree_nodes.end())
          return finder->second;
      }
      // Couldn't find it, so send a request to the owner node
      AddressSpaceID owner = RegionTreeNode::get_owner_space(tid, runtime);
      if (owner == runtime->address_space)
      {
        log_run.error("Unable to find entry for region tree ID %d", tid);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_TREE_ENTRY);
      }
      RtEvent wait_on;
      {
        // Retake the lock in exclusive mode and check to
        // make sure that we didn't lose the race
        AutoLock l_lock(lookup_lock);
        std::map<RegionTreeID,RegionNode*>::const_iterator finder = 
          tree_nodes.find(tid);
        if (finder != tree_nodes.end())
          return finder->second;
        // Now see if we've already send a request
        std::map<RegionTreeID,RtEvent>::const_iterator req_finder =
          region_tree_requests.find(tid);
        if (req_finder == region_tree_requests.end())
        {
          RtUserEvent done = Runtime::create_rt_user_event();
          region_tree_requests[tid] = done;
          Serializer rez;
          rez.serialize(tid);
          rez.serialize(done);
          runtime->send_top_level_region_request(owner, rez);
          wait_on = done;
        }
        else
          wait_on = req_finder->second;
      }
      wait_on.wait();
      AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
      std::map<RegionTreeID,RegionNode*>::const_iterator finder = 
          tree_nodes.find(tid);
      if (finder == tree_nodes.end())
      {
        log_region.error("Unable to find top-level tree entry for "
                         "region tree %d.  This is either a runtime "
                         "bug or requires Legion fences if names are "
                         "being returned out of the context in which"
                         "they are being created.", tid);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_TREE_ENTRY);
      }
      return finder->second;
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_node(IndexSpace space, bool local_only)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
        if (index_nodes.find(space) != index_nodes.end())
          return true;
        if (local_only)
          return false;
      }
      return (get_node(space) != NULL);
    }
    
    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_node(IndexPartition part, bool local_only)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
        if (index_parts.find(part) != index_parts.end())
          return true;
        if (local_only)
          return false;
      }
      return (get_node(part) != NULL);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_node(FieldSpace space, bool local_only)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
        if (field_nodes.find(space) != field_nodes.end())
          return true;
        if (local_only)
          return false;
      }
      return (get_node(space) != NULL);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_node(LogicalRegion handle, bool local_only)
    //--------------------------------------------------------------------------
    {
      // Reflect that we can build these nodes whenever this is true
      return (has_node(handle.index_space, local_only) && 
              has_node(handle.field_space, local_only) &&
              has_tree(handle.tree_id, local_only));
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_node(LogicalPartition handle, bool local_only)
    //--------------------------------------------------------------------------
    {
      // Reflect that we can build these nodes whenever this is true
      return (has_node(handle.index_partition, local_only) && 
              has_node(handle.field_space, local_only)
              && has_tree(handle.tree_id, local_only));
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_tree(RegionTreeID tid, bool local_only)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock l_lock(lookup_lock,1,false/*exclusive*/);
        if (tree_nodes.find(tid) != tree_nodes.end())
          return true;
        if (local_only)
          return false;
      }
      return (get_tree(tid) != NULL);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::has_field(FieldSpace space, FieldID fid)
    //--------------------------------------------------------------------------
    {
      return get_node(space)->has_field(fid);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::is_top_level_index_space(IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      return (get_node(handle)->parent == NULL);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::is_top_level_region(LogicalRegion handle)
    //--------------------------------------------------------------------------
    {
      return (get_node(handle)->parent == NULL);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::is_subregion(LogicalRegion child, 
                                        LogicalRegion parent)
    //--------------------------------------------------------------------------
    {
      if (child == parent)
        return true;
      if (child.get_tree_id() != parent.get_tree_id())
        return false;
      std::vector<ColorPoint> path;
      return compute_index_path(parent.get_index_space(),
                                child.get_index_space(), path);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::is_disjoint(IndexPartition handle)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *node = get_node(handle);
      return node->is_disjoint(true/*app query*/);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::is_disjoint(LogicalPartition handle)
    //--------------------------------------------------------------------------
    {
      return is_disjoint(handle.get_index_partition());
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::are_disjoint(IndexSpace one, IndexSpace two)
    //--------------------------------------------------------------------------
    {
      if (one == two)
        return false;
      if (one.get_tree_id() != two.get_tree_id())
        return true;
      // See if they intersect with each other
      IndexSpaceNode *sp_one = get_node(one);
      IndexSpaceNode *sp_two = get_node(two);
      return !sp_one->intersects_with(sp_two);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::are_disjoint(IndexSpace one, IndexPartition two)
    //--------------------------------------------------------------------------
    {
      if (one.get_tree_id() != two.get_tree_id())
        return true;
      IndexSpaceNode *space_node = get_node(one);
      IndexPartNode *part_node = get_node(two);
      return !space_node->intersects_with(part_node);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::are_disjoint(IndexPartition one, IndexPartition two)
    //--------------------------------------------------------------------------
    {
      if (one == two)
        return false;
      if (one.get_tree_id() != two.get_tree_id())
        return true;
      IndexPartNode *part_one = get_node(one);
      IndexPartNode *part_two = get_node(two);
      return !part_one->intersects_with(part_two);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::are_compatible(IndexSpace left, IndexSpace right)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *left_node = get_node(left);
      IndexSpaceNode *right_node = get_node(right);
      const Domain &left_dom = left_node->get_domain_blocking();
      const Domain &right_dom = right_node->get_domain_blocking();
      if (left_dom.get_dim() != right_dom.get_dim())
        return false;
      return true;
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::is_dominated(IndexSpace src, IndexSpace dst)
    //--------------------------------------------------------------------------
    {
      // Check to see if dst is dominated by source
#ifdef DEBUG_LEGION
      assert(are_compatible(src, dst));
#endif
      IndexSpaceNode *src_node = get_node(src);
      IndexSpaceNode *dst_node = get_node(dst);
      return src_node->dominates(dst_node);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::compute_index_path(IndexSpace parent, 
                               IndexSpace child, std::vector<ColorPoint> &path)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *child_node = get_node(child); 
      path.push_back(child_node->color);
      if (parent == child) 
        return true; // Early out
      IndexSpaceNode *parent_node = get_node(parent);
      while (parent_node != child_node)
      {
        if (parent_node->depth >= child_node->depth)
        {
          path.clear();
          return false;
        }
        if (child_node->parent == NULL)
        {
          path.clear();
          return false;
        }
        path.push_back(child_node->parent->color);
        path.push_back(child_node->parent->parent->color);
        child_node = child_node->parent->parent;
      }
      return true;
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::compute_partition_path(IndexSpace parent, 
                           IndexPartition child, std::vector<ColorPoint> &path)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *child_node = get_node(child);
      path.push_back(child_node->color);
      if (child_node->parent == NULL)
      {
        path.clear();
        return false;
      }
      return compute_index_path(parent, child_node->parent->handle, path);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::initialize_path(IndexSpace child, IndexSpace parent,
                                           RegionTreePath &path)
    //--------------------------------------------------------------------------
    {
      initialize_path(get_node(child), get_node(parent), path);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::initialize_path(IndexPartition child, 
                                           IndexSpace parent, 
                                           RegionTreePath &path)
    //--------------------------------------------------------------------------
    {
      initialize_path(get_node(child), get_node(parent), path);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::initialize_path(IndexSpace child,
                                           IndexPartition parent,
                                           RegionTreePath &path)
    //--------------------------------------------------------------------------
    {
      initialize_path(get_node(child), get_node(parent), path);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::initialize_path(IndexPartition child,
                                           IndexPartition parent,
                                           RegionTreePath &path)
    //--------------------------------------------------------------------------
    {
      initialize_path(get_node(child), get_node(parent), path);
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::initialize_path(IndexTreeNode *child,
                                           IndexTreeNode *parent,
                                           RegionTreePath &path)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(child->depth >= parent->depth);
#endif
      path.initialize(parent->depth, child->depth);
      while (child != parent)
      {
#ifdef DEBUG_LEGION
        assert(child->depth > 0);
#endif
        path.register_child(child->depth-1,child->color);
        child = child->get_parent();
      }
    }

    //--------------------------------------------------------------------------
    FatTreePath* RegionTreeForest::compute_fat_path(IndexSpace child,
                                                    IndexSpace parent,
                                 std::map<IndexTreeNode*,FatTreePath*> &storage,
                                                    bool test, bool &overlap)
    //--------------------------------------------------------------------------
    {
      IndexTreeNode *child_node = get_node(child);
      IndexTreeNode *parent_node = get_node(parent);
      return compute_fat_path(child_node, parent_node, storage, test, overlap);
    }

    //--------------------------------------------------------------------------
    FatTreePath* RegionTreeForest::compute_fat_path(IndexSpace child,
                                                    IndexPartition parent,
                                 std::map<IndexTreeNode*,FatTreePath*> &storage,
                                                    bool test, bool &overlap)
    //--------------------------------------------------------------------------
    {
      IndexTreeNode *child_node = get_node(child);
      IndexTreeNode *parent_node = get_node(parent);
      return compute_fat_path(child_node, parent_node, storage, test, overlap);
    }

    //--------------------------------------------------------------------------
    FatTreePath* RegionTreeForest::compute_full_fat_path(IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      IndexSpaceNode *node = get_node(handle);
      return compute_full_fat_path(node); 
    }

    //--------------------------------------------------------------------------
    FatTreePath* RegionTreeForest::compute_full_fat_path(IndexPartition handle)
    //--------------------------------------------------------------------------
    {
      IndexPartNode *node = get_node(handle);
      return compute_full_fat_path(node);
    }

    //--------------------------------------------------------------------------
    FatTreePath* RegionTreeForest::compute_fat_path(IndexTreeNode *child,
                                                    IndexTreeNode *parent,
                                 std::map<IndexTreeNode*,FatTreePath*> &storage,
                                               bool test_overlap, bool &overlap)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(parent->depth <= child->depth); // some sanity checking
#endif
      if (storage.find(child) != storage.end())
      {
        if (test_overlap)
          overlap = true;
        std::map<IndexTreeNode*,FatTreePath*>::const_iterator finder = 
          storage.find(parent);
#ifdef DEBUG_LEGION
        assert(finder != storage.end());
#endif
        return finder->second;
      }
      if (child == parent)
      {
        FatTreePath *result = new FatTreePath();
        storage[child] = result;
        if (test_overlap)
          overlap = false;
        return result;
      }
      IndexTreeNode *current = child;
      FatTreePath *current_path = new FatTreePath();
      // Add ourselves to the map
      storage[current] = current_path;
      while (current != parent)
      {
#ifdef DEBUG_LEGION
        assert(parent->depth < child->depth);
#endif
        IndexTreeNode *next = current->get_parent();
#ifdef DEBUG_LEGION
        assert(next != NULL);
#endif
        std::map<IndexTreeNode*,FatTreePath*>::const_iterator finder = 
          storage.find(next);
        if (finder != storage.end())
        {
          // If we found it then add it and check for disjointness
          if (test_overlap)
            overlap = finder->second->add_child(current->color, 
                                                current_path, next);
          else
            finder->second->add_child(current->color, current_path);
          if (next == parent)
            return finder->second;
          finder = storage.find(parent);
#ifdef DEBUG_LEGION
          assert(finder != storage.end()); 
#endif
          return finder->second;
        }
        // Otherwise the next one doesn't exist yet so make it 
        FatTreePath *next_path = new FatTreePath();
        storage[next] = next_path;
        next_path->add_child(current->color, current_path);
        current = next;
        current_path = next_path;
      }
      // We had to add the whole path so we are done
      if (test_overlap)
        overlap = false;
      return current_path;
    }

    //--------------------------------------------------------------------------
    FatTreePath* RegionTreeForest::compute_full_fat_path(IndexSpaceNode *node)
    //--------------------------------------------------------------------------
    {
      std::map<ColorPoint,IndexPartNode*> children;
      node->get_children(children);
      FatTreePath *result = new FatTreePath();
      for (std::map<ColorPoint,IndexPartNode*>::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        FatTreePath *child_path = compute_full_fat_path(it->second);
        result->add_child(it->first, child_path);
      }
      return result;
    }

    //--------------------------------------------------------------------------
    FatTreePath* RegionTreeForest::compute_full_fat_path(IndexPartNode *node)
    //--------------------------------------------------------------------------
    {
      std::map<ColorPoint,IndexSpaceNode*> children;
      node->get_children(children);
      FatTreePath *result = new FatTreePath();
      for (std::map<ColorPoint,IndexSpaceNode*>::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        FatTreePath *child_path = compute_full_fat_path(it->second);
        result->add_child(it->first, child_path);
      }
      return result;
    }

    //--------------------------------------------------------------------------
    PhysicalInstance RegionTreeForest::create_instance(const Domain &dom,
                    Memory target, const std::vector<size_t> &field_sizes, 
                    size_t blocking_factor, ReductionOpID redop, UniqueID op_id)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REALM_CREATE_INSTANCE_CALL);
      if (runtime->profiler != NULL)
      {
        Realm::ProfilingRequestSet reqs;
        runtime->profiler->add_inst_request(reqs, op_id);
        PhysicalInstance result = dom.create_instance(target, field_sizes, 
                                            blocking_factor, reqs, redop);
        // If the result exists tell the profiler about it in case
        // it never gets deleted and we never see the profiling feedback
        if (result.exists())
        {
          unsigned long long creation_time = 
            Realm::Clock::current_time_in_nanoseconds();
          runtime->profiler->record_instance_creation(result, target, op_id,
                                                      creation_time);
        }
        return result;
      }
      else
        return dom.create_instance(target, field_sizes, 
                                   blocking_factor, redop);
    }

    //--------------------------------------------------------------------------
    template<typename T>
    Color RegionTreeForest::generate_unique_color(
                                          const std::map<Color,T> &current_map)
    //--------------------------------------------------------------------------
    {
      if (current_map.empty())
        return runtime->get_start_color();
      unsigned stride = runtime->get_color_modulus();
      typename std::map<Color,T>::const_reverse_iterator rlast = current_map.rbegin();
      Color result = rlast->first + stride;
#ifdef DEBUG_LEGION
      assert(current_map.find(result) == current_map.end());
#endif
      return result;
    }

#ifdef DEBUG_LEGION
    //--------------------------------------------------------------------------
    void RegionTreeForest::dump_logical_state(LogicalRegion region,
                                              ContextID ctx)
    //--------------------------------------------------------------------------
    {
      TreeStateLogger dump_logger; 
      assert(region_nodes.find(region) != region_nodes.end());
      region_nodes[region]->dump_logical_context(ctx, &dump_logger,
                                 FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES));
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::dump_physical_state(LogicalRegion region,
                                               ContextID ctx)
    //--------------------------------------------------------------------------
    {
      TreeStateLogger dump_logger;
      assert(region_nodes.find(region) != region_nodes.end());
      region_nodes[region]->dump_physical_context(ctx, &dump_logger,
                                FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES));
    }
#endif

    //--------------------------------------------------------------------------
    void RegionTreeForest::attach_semantic_information(IndexSpace handle,
                                                       SemanticTag tag,
                                                       AddressSpaceID source,
                                                       const void *buffer,
                                                       size_t size, 
                                                       bool is_mutable)
    //--------------------------------------------------------------------------
    {
      get_node(handle)->attach_semantic_information(tag, source, buffer, 
                                                    size, is_mutable);
      if (Runtime::legion_spy_enabled && (NAME_SEMANTIC_TAG == tag))
        LegionSpy::log_index_space_name(handle.id,
            reinterpret_cast<const char*>(buffer));
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::attach_semantic_information(IndexPartition handle,
                                                       SemanticTag tag,
                                                       AddressSpaceID source,
                                                       const void *buffer,
                                                       size_t size,
                                                       bool is_mutable)
    //--------------------------------------------------------------------------
    {
      get_node(handle)->attach_semantic_information(tag, source, buffer, 
                                                    size, is_mutable);
      if (Runtime::legion_spy_enabled && (NAME_SEMANTIC_TAG == tag))
        LegionSpy::log_index_partition_name(handle.id,
            reinterpret_cast<const char*>(buffer));
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::attach_semantic_information(FieldSpace handle,
                                                       SemanticTag tag,
                                                       AddressSpaceID source,
                                                       const void *buffer,
                                                       size_t size,
                                                       bool is_mutable)
    //--------------------------------------------------------------------------
    {
      get_node(handle)->attach_semantic_information(tag, source, buffer, 
                                                    size, is_mutable);
      if (Runtime::legion_spy_enabled && (NAME_SEMANTIC_TAG == tag))
        LegionSpy::log_field_space_name(handle.id,
            reinterpret_cast<const char*>(buffer));
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::attach_semantic_information(FieldSpace handle,
                                                       FieldID fid,
                                                       SemanticTag tag,
                                                       AddressSpaceID src,
                                                       const void *buf,
                                                       size_t size,
                                                       bool is_mutable)
    //--------------------------------------------------------------------------
    {
      get_node(handle)->attach_semantic_information(fid, tag, src, buf, 
                                                    size, is_mutable);
      if (Runtime::legion_spy_enabled && (NAME_SEMANTIC_TAG == tag))
        LegionSpy::log_field_name(handle.id, fid,
            reinterpret_cast<const char*>(buf));
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::attach_semantic_information(LogicalRegion handle,
                                                       SemanticTag tag,
                                                       AddressSpaceID source,
                                                       const void *buffer,
                                                       size_t size,
                                                       bool is_mutable)
    //--------------------------------------------------------------------------
    {
      get_node(handle)->attach_semantic_information(tag, source, buffer, 
                                                    size, is_mutable);
      if (Runtime::legion_spy_enabled && (NAME_SEMANTIC_TAG == tag))
        LegionSpy::log_logical_region_name(handle.index_space.id,
            handle.field_space.id, handle.tree_id,
            reinterpret_cast<const char*>(buffer));
    }

    //--------------------------------------------------------------------------
    void RegionTreeForest::attach_semantic_information(LogicalPartition handle,
                                                       SemanticTag tag,
                                                       AddressSpaceID source,
                                                       const void *buffer,
                                                       size_t size,
                                                       bool is_mutable)
    //--------------------------------------------------------------------------
    {
      get_node(handle)->attach_semantic_information(tag, source, buffer, 
                                                    size, is_mutable);
      if (Runtime::legion_spy_enabled && (NAME_SEMANTIC_TAG == tag))
        LegionSpy::log_logical_partition_name(handle.index_partition.id,
            handle.field_space.id, handle.tree_id,
            reinterpret_cast<const char*>(buffer));
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::retrieve_semantic_information(IndexSpace handle,
                                                         SemanticTag tag,
                                                         const void *&result,
                                                         size_t &size,
                                                         bool can_fail,
                                                         bool wait_until)
    //--------------------------------------------------------------------------
    {
      return get_node(handle)->retrieve_semantic_information(tag, result, size,
                                                          can_fail, wait_until);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::retrieve_semantic_information(IndexPartition handle,
                                                         SemanticTag tag,
                                                         const void *&result,
                                                         size_t &size,
                                                         bool can_fail,
                                                         bool wait_until)
    //--------------------------------------------------------------------------
    {
      return get_node(handle)->retrieve_semantic_information(tag, result, size,
                                                          can_fail, wait_until);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::retrieve_semantic_information(FieldSpace handle,
                                                         SemanticTag tag,
                                                         const void *&result,
                                                         size_t &size,
                                                         bool can_fail,
                                                         bool wait_until)
    //--------------------------------------------------------------------------
    {
      return get_node(handle)->retrieve_semantic_information(tag, result, size,
                                                         can_fail, wait_until);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::retrieve_semantic_information(FieldSpace handle,
                                                         FieldID fid,
                                                         SemanticTag tag,
                                                         const void *&result,
                                                         size_t &size,
                                                         bool can_fail,
                                                         bool wait_until)
    //--------------------------------------------------------------------------
    {
      return get_node(handle)->retrieve_semantic_information(fid, tag, result, 
                                                  size, can_fail, wait_until);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::retrieve_semantic_information(LogicalRegion handle,
                                                         SemanticTag tag,
                                                         const void *&result,
                                                         size_t &size,
                                                         bool can_fail,
                                                         bool wait_until)
    //--------------------------------------------------------------------------
    {
      return get_node(handle)->retrieve_semantic_information(tag, result, size,
                                                         can_fail, wait_until);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeForest::retrieve_semantic_information(LogicalPartition part,
                                                         SemanticTag tag,
                                                         const void *&result,
                                                         size_t &size,
                                                         bool can_fail,
                                                         bool wait_until)
    //--------------------------------------------------------------------------
    {
      return get_node(part)->retrieve_semantic_information(tag, result, size,
                                                         can_fail, wait_until);
    }

    //--------------------------------------------------------------------------
    /*static*/ bool RegionTreeForest::are_disjoint(const Domain &left,
                                                   const Domain &right)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(left.get_dim() == right.get_dim());
#endif
      bool disjoint = true;
      switch (left.get_dim())
      {
        case 0:
          {
            const Realm::ElementMask &left_mask = 
                                       left.get_index_space().get_valid_mask();
            const Realm::ElementMask &right_mask = 
                                       right.get_index_space().get_valid_mask();
            Realm::ElementMask::OverlapResult result = 
                                            left_mask.overlaps_with(right_mask);
            if (result != Realm::ElementMask::OVERLAP_NO)
              disjoint = false;
            break;
          }
        case 1:
          {
            Rect<1> leftr = left.get_rect<1>();
            Rect<1> rightr = right.get_rect<1>();
            if (leftr.overlaps(rightr))
              disjoint = false;
            break;
          }
        case 2:
          {
            Rect<2> leftr = left.get_rect<2>();
            Rect<2> rightr = right.get_rect<2>();
            if (leftr.overlaps(rightr))
              disjoint = false;
            break;
          }
        case 3:
          {
            Rect<3> leftr = left.get_rect<3>();
            Rect<3> rightr = right.get_rect<3>();
            if (leftr.overlaps(rightr))
              disjoint = false;
            break;
          }
        default:
          assert(false);
      }
      return disjoint;
    }

    //--------------------------------------------------------------------------
    /*static*/ bool RegionTreeForest::are_disjoint(IndexSpaceNode *left,
                                                   IndexSpaceNode *right)
    //--------------------------------------------------------------------------
    {
      bool disjoint = true;
      if (left->has_component_domains())
      {
        const std::set<Domain> &left_domains = 
          left->get_component_domains_blocking();
        if (right->has_component_domains())
        {
          const std::set<Domain> &right_domains = 
            right->get_component_domains_blocking();
          // Double Loop
          for (std::set<Domain>::const_iterator lit = left_domains.begin();
                disjoint && (lit != left_domains.end()); lit++)
          {
            for (std::set<Domain>::const_iterator rit = right_domains.begin();
                  disjoint && (rit != right_domains.end()); rit++)
            {
              disjoint = RegionTreeForest::are_disjoint(*lit, *rit);
            }
          }
        }
        else
        {
          // Loop over left components
          for (std::set<Domain>::const_iterator it = left_domains.begin();
                disjoint && (it != left_domains.end()); it++)
          {
            disjoint = RegionTreeForest::are_disjoint(*it, 
                        right->get_domain_blocking());
          }
        }
      }
      else
      {
        if (right->has_component_domains())
        {
          const std::set<Domain> &right_domains = 
              right->get_component_domains_blocking();
          // Loop over right components
          for (std::set<Domain>::const_iterator it = right_domains.begin();
                disjoint && (it != right_domains.end()); it++)
          {
            disjoint = RegionTreeForest::are_disjoint(
                          left->get_domain_blocking(), *it);
          }
        }
        else
        {
          // No Loops
          disjoint = RegionTreeForest::are_disjoint(left->get_domain_blocking(),
                                                  right->get_domain_blocking());
        }
      }
      return disjoint;
    }

    /////////////////////////////////////////////////////////////
    // Index Tree Node 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    IndexTreeNode::IndexTreeNode(void)
      : depth(0), color(ColorPoint()), context(NULL), destroyed(false)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    IndexTreeNode::IndexTreeNode(ColorPoint c, unsigned d, 
                                 RegionTreeForest *ctx)
      : depth(d), color(c), context(ctx), destroyed(false),
        node_lock(Reservation::create_reservation())
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(ctx != NULL);
#endif
    }

    //--------------------------------------------------------------------------
    IndexTreeNode::~IndexTreeNode(void)
    //--------------------------------------------------------------------------
    {
      node_lock.destroy_reservation();
      node_lock = Reservation::NO_RESERVATION;
      for (LegionMap<SemanticTag,SemanticInfo>::aligned::iterator it = 
            semantic_info.begin(); it != semantic_info.end(); it++)
      {
        legion_free(SEMANTIC_INFO_ALLOC, it->second.buffer, it->second.size);
      }
      for (std::map<IndexTreeNode*,IntersectInfo>::iterator it = 
            intersections.begin(); it != intersections.end(); it++)
      {
        IntersectInfo &info = it->second; 
        for (std::set<Domain>::iterator dit = info.intersections.begin();
              dit != info.intersections.end(); dit++)
        {
          Realm::IndexSpace space = dit->get_index_space();
          if (space.exists())
            space.destroy();
        }
      }
      intersections.clear();
    }

    //--------------------------------------------------------------------------
    void IndexTreeNode::attach_semantic_information(SemanticTag tag,
                                                    AddressSpaceID source,
                                                    const void *buffer, 
                                                    size_t size,bool is_mutable)
    //--------------------------------------------------------------------------
    {
      // Make a copy
      void *local = legion_malloc(SEMANTIC_INFO_ALLOC, size);
      memcpy(local, buffer, size);
      bool added = true;
      RtUserEvent to_trigger;
      {
        AutoLock n_lock(node_lock); 
        // See if it already exists
        LegionMap<SemanticTag,SemanticInfo>::aligned::iterator finder = 
          semantic_info.find(tag);
        if (finder != semantic_info.end())
        {
          // First check to see if it is valid
          if (finder->second.is_valid())
          {
            if (!finder->second.is_mutable)
            {
              // It's not mutable so check to make 
              // sure that the bits are the same
              if (size != finder->second.size)
              {
                log_run.error("ERROR: Inconsistent Semantic Tag value "
                              "for tag %ld with different sizes of %ld"
                              " and %ld for index tree node", 
                              tag, size, finder->second.size);
#ifdef DEBUG_LEGION
                assert(false);
#endif
                exit(ERROR_INCONSISTENT_SEMANTIC_TAG);       
              }
              // Otherwise do a bitwise comparison
              {
                const char *orig = (const char*)finder->second.buffer;
                const char *next = (const char*)buffer;
                for (unsigned idx = 0; idx < size; idx++)
                {
                  char diff = orig[idx] ^ next[idx];
                  if (diff)
                  {
                    log_run.error("ERROR: Inconsistent Semantic Tag value "
                                  "for tag %ld with different values at"
                                  "byte %d for index tree node, %x != %x",
                                  tag, idx, orig[idx], next[idx]);
#ifdef DEBUG_LEGION
                    assert(false);
#endif
                    exit(ERROR_INCONSISTENT_SEMANTIC_TAG);
                  }
                }
              }
              added = false;
            }
            else
            {
              // Mutable so overwrite the result
              legion_free(SEMANTIC_INFO_ALLOC, finder->second.buffer,
                          finder->second.size);
              finder->second.buffer = local;
              finder->second.size = size;
              finder->second.ready_event = RtUserEvent::NO_RT_USER_EVENT;
              finder->second.is_mutable = is_mutable;
            }
          }
          else
          {
            finder->second.buffer = local;
            finder->second.size = size;
            // See if we have an event to trigger
            to_trigger = finder->second.ready_event;
            finder->second.ready_event = RtUserEvent::NO_RT_USER_EVENT;
            finder->second.is_mutable = is_mutable;
          }
        }
        else
          semantic_info[tag] = SemanticInfo(local, size, is_mutable);
      }
      // Trigger the ready event if there is one
      if (to_trigger.exists())
        Runtime::trigger_event(to_trigger);
      if (added)
      {
        AddressSpaceID owner_space = get_owner_space();
        // If we are not the owner and the message 
        // didn't come from the owner, then send it 
        if ((owner_space != context->runtime->address_space) &&
            (source != owner_space))
        {
          send_semantic_info(owner_space, tag, buffer, size, is_mutable); 
        }
      }
      else
        legion_free(SEMANTIC_INFO_ALLOC, local, size);
    }

    //--------------------------------------------------------------------------
    bool IndexTreeNode::retrieve_semantic_information(SemanticTag tag,
                                                      const void *&result,
                                                      size_t &size, 
                                                      bool can_fail,
                                                      bool wait_until)
    //--------------------------------------------------------------------------
    {
      RtUserEvent wait_on;
      {
        AutoLock n_lock(node_lock);
        LegionMap<SemanticTag,SemanticInfo>::aligned::const_iterator finder = 
          semantic_info.find(tag); 
        if (finder != semantic_info.end())
        {
          // Already have the data so we are done
          if (finder->second.is_valid())
          {
            result = finder->second.buffer;
            size = finder->second.size;
            return true;
          }
          else if (!can_fail && wait_until)
            wait_on = finder->second.ready_event;
          else // we can fail, so make our user event
            wait_on = Runtime::create_rt_user_event();
        }
        else
        {
          if (!can_fail && wait_until)
          {
            // Otherwise make an event to wait on
            wait_on = Runtime::create_rt_user_event();
            semantic_info[tag] = SemanticInfo(wait_on);
          }
          else
            wait_on = Runtime::create_rt_user_event();
        }
      }
      // If we are not the owner, send a request, otherwise we are
      // the owner and the information will get sent here
      AddressSpaceID owner_space = get_owner_space();
      if (owner_space != context->runtime->address_space)
        send_semantic_request(owner_space, tag, can_fail, wait_until, wait_on);
      else
      {
        if (can_fail)
          return false;
        log_run.error("ERROR: invalid semantic tag %ld for "
                      "index tree node", tag);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_SEMANTIC_TAG);
      }
      // Now wait
      wait_on.wait();
      // When we wake up, we should be able to find everything
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      LegionMap<SemanticTag,SemanticInfo>::aligned::const_iterator finder = 
        semantic_info.find(tag);
      if (finder == semantic_info.end())
      {
        if (can_fail)
          return false;
        log_run.error("ERROR: invalid semantic tag %ld for "
                            "index tree node", tag);   
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_SEMANTIC_TAG);
      }
      result = finder->second.buffer;
      size = finder->second.size;
      return true;
    }

    //--------------------------------------------------------------------------
    /*static*/ bool IndexTreeNode::compute_intersections(
        const std::set<Domain> &left, const std::set<Domain> &right, 
        std::set<Domain> &result_domains, bool compute)
    //--------------------------------------------------------------------------
    {
      for (std::set<Domain>::const_iterator lit = left.begin();
            lit != left.end(); lit++)
      {
        for (std::set<Domain>::const_iterator rit = right.begin();
              rit != right.end(); rit++)
        {
          Domain result;
          if (compute_intersection(*lit, *rit, result, compute))
          {
            if (compute)
              result_domains.insert(result);
            else
              return true;
          }
        }
      }
      return !result_domains.empty();
    }

    //--------------------------------------------------------------------------
    /*static*/ bool IndexTreeNode::compute_intersections(
        const std::set<Domain> &left, const Domain &right, 
        std::set<Domain> &result_domains, bool compute)
    //--------------------------------------------------------------------------
    {
      for (std::set<Domain>::const_iterator it = left.begin();
            it != left.end(); it++)
      {
        Domain result;
        if (compute_intersection(*it, right, result, compute))
        {
          if (compute)
            result_domains.insert(result);
          else
            return true;
        }
      }
      return !result_domains.empty();
    }

    //--------------------------------------------------------------------------
    /*static*/ bool IndexTreeNode::compute_intersection(const Domain &left,
                                                        const Domain &right,
                                                        Domain &result,
                                                        bool compute)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(left.get_dim() == right.get_dim());
#endif
      bool non_empty = false;
      switch (left.get_dim())
      {
        case 0:
          {
            const Realm::ElementMask &left_mask = 
                                    left.get_index_space().get_valid_mask();
            const Realm::ElementMask &right_mask = 
                                    right.get_index_space().get_valid_mask();
            Realm::ElementMask intersection = left_mask & right_mask;
            if (!!intersection)
            {
              non_empty = true;
              if (compute)
                result = 
                 Domain(Realm::IndexSpace::create_index_space(intersection));
            }
            break;
          }
        case 1:
          {
            Rect<1> leftr = left.get_rect<1>();
            Rect<1> rightr = right.get_rect<1>();
            Rect<1> temp = leftr.intersection(rightr);
            if (temp.volume() > 0)
            {
              non_empty = true;
              if (compute)
                result = Domain::from_rect<1>(temp);
            }
            break;
          }
        case 2:
          {
            Rect<2> leftr = left.get_rect<2>();
            Rect<2> rightr = right.get_rect<2>();
            Rect<2> temp = leftr.intersection(rightr);
            if (temp.volume() > 0)
            {
              non_empty = true;
              if (compute)
                result = Domain::from_rect<2>(temp);
            }
            break;
          }
        case 3:
          {
            Rect<3> leftr = left.get_rect<3>();
            Rect<3> rightr = right.get_rect<3>();
            Rect<3> temp = leftr.intersection(rightr);
            if (temp.volume() > 0)
            {
              non_empty = true;
              if (compute)
                result = Domain::from_rect<3>(temp);
            }
            break;
          }
        default:
          assert(false);
      }
      return non_empty;
    }

    //--------------------------------------------------------------------------
    /*static*/ bool IndexTreeNode::compute_dominates(
            const std::set<Domain> &left_set, const std::set<Domain> &right_set)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!left_set.empty());
      assert(!right_set.empty());
#endif
      // Check to see if the left set of domains dominates the right set
      bool dominates = false;
      // Handle the easy case for dimension zero
      Domain left = *(left_set.begin());
      if (left.get_dim() == 0)
      {
	// We're going to compute the union of the right set members and then
	//  subtract out the left set members and see if anything is left
	// If there is, left does NOT dominate right
	
	Realm::ElementMask *mask = 0;
	// We need first to make sure we have an ElementMask that can hold all
	//  the right_set members, which may have been trimmed
	if (right_set.size() == 1)
	{
	  // just make a copy of the only set member's mask
	  mask = new Realm::ElementMask(right_set.begin()->get_index_space()
					   .get_valid_mask());
	} else {
	  std::set<Domain>::const_iterator it = right_set.begin();
	  assert(it != right_set.end());
	  const Realm::ElementMask *maskp = 
            &(it->get_index_space().get_valid_mask());
	  int first_elmt = maskp->first_enabled();
	  int last_elmt = maskp->last_enabled();
	  while(++it != right_set.end())
	  {
	    maskp = &(it->get_index_space().get_valid_mask());
	    int new_first = maskp->first_enabled();
	    int new_last = maskp->last_enabled();
	    if ((new_first != -1) && 
                ((first_elmt == -1) || (first_elmt > new_first)))
	      first_elmt = new_first;
	    if ((new_last != -1) && (last_elmt < new_last))
	      last_elmt = new_last;
	  }
	  // If there are no elements, right is trivially dominated
	  if ((first_elmt > last_elmt) || (last_elmt == -1))
	    return true;
	  // Now construct the mask
	  mask = new Realm::ElementMask(last_elmt - first_elmt + 1, first_elmt);
	  // And copy in the bits from each member set
	  for (it = right_set.begin(); it != right_set.end(); it++)
	    *mask |= it->get_index_space().get_valid_mask();
	}

	// Now go through the left set members and subtract them all ot
	for (std::set<Domain>::const_iterator it = left_set.begin();
	     it != left_set.end(); it++)
	  *mask -= it->get_index_space().get_valid_mask();
	  
        // Union left and right together and then test (empty == domainated)
	dominates = !(*mask);

	// Clean up our working copy
	delete mask;
      }
      else if (left_set.size() == 1)
      {
        // This is the easy case where we only have a single domain on the left
        switch (left.get_dim())
        {
          case 1:
            {
              Rect<1> leftr = left.get_rect<1>();
              dominates = true;
              for (std::set<Domain>::const_iterator it = right_set.begin();
                    it != right_set.end(); it++)
              {
                Rect<1> right = it->get_rect<1>(); 
                if ((right.intersection(leftr)) != right)
                {
                  dominates = false;
                  break;
                }
              }
              break;
            }
          case 2:
            {
              Rect<2> leftr = left.get_rect<2>();
              dominates = true;
              for (std::set<Domain>::const_iterator it = right_set.begin();
                    it != right_set.end(); it++)
              {
                Rect<2> right = it->get_rect<2>(); 
                if ((right.intersection(leftr)) != right)
                {
                  dominates = false;
                  break;
                }
              }
              break;
            }
          case 3:
            {
              Rect<3> leftr = left.get_rect<3>();
              dominates = true;
              for (std::set<Domain>::const_iterator it = right_set.begin();
                    it != right_set.end(); it++)
              {
                Rect<3> right = it->get_rect<3>(); 
                if ((right.intersection(leftr)) != right)
                {
                  dominates = false;
                  break;
                }
              }
              break;
            }
          default:
            assert(false);
        }
      }
      else
      {
        // This is the hard case where we have multiple domains on the left
        switch (left.get_dim())
        {
          case 1:
            {
              // Construct an interval tree for the left set
              // and then check to see if all the intervals within
              // the right set are dominated by an interval in the tree
              IntervalTree<int,true/*discrete*/> intervals;
              for (std::set<Domain>::const_iterator it = left_set.begin();
                    it != left_set.end(); it++)
              {
                Rect<1> left_rect = it->get_rect<1>();
                intervals.insert(left_rect.lo[0], left_rect.hi[0]);
              }
              dominates = true;
              for (std::set<Domain>::const_iterator it = right_set.begin();
                    it != right_set.end(); it++)
              {
                Rect<1> right_rect = it->get_rect<1>();
                if (!intervals.dominates(right_rect.lo[0], right_rect.hi[0]))
                {
                  dominates = false;
                  break;
                }
              }
              break;
            }
          case 2:
            {
              RectangleSet<int,true/*discrete*/> rectangles;
              for (std::set<Domain>::const_iterator it = left_set.begin();
                    it != left_set.end(); it++)
              {
                Rect<2> left_rect = it->get_rect<2>();
                if (left_rect.volume() > 0)
                  rectangles.add_rectangle(left_rect.lo[0], left_rect.lo[1],
                                           left_rect.hi[0], left_rect.hi[1]);
              }
              dominates = true;
              for (std::set<Domain>::const_iterator it = right_set.begin();
                    it != right_set.end(); it++)
              {
                Rect<2> right_rect = it->get_rect<2>();
                if (right_rect.volume() > 0 &&
                    !rectangles.covers(right_rect.lo[0], right_rect.lo[1],
                                       right_rect.hi[0], right_rect.hi[1]))
                {
                  dominates = false;
                  break;
                }
              }
              break;
            }
          case 3:
            {
              // TODO: Improve this terrible approximation
              dominates = true;
              for (std::set<Domain>::const_iterator rit = right_set.begin();
                    (rit != right_set.end()) && dominates; rit++)
              {
                Rect<3> right_rect = rit->get_rect<3>();
                bool has_dominator = false;
                // See if any of the rectangles on the left dominate it
                for (std::set<Domain>::const_iterator lit = left_set.begin();
                      lit != left_set.end(); lit++)
                {
                  Rect<3> left_rect = lit->get_rect<3>();
                  if (right_rect.intersection(left_rect) == right_rect)
                  {
                    has_dominator = true;
                    break;
                  }
                }
                if (!has_dominator)
                {
                  dominates = false;
                  break;
                }
              }
              break;
            }
          default:
            assert(false); // should never get here
        }
      }
      return dominates;
    }


    /////////////////////////////////////////////////////////////
    // Index Space Node 
    /////////////////////////////////////////////////////////////
    
    //--------------------------------------------------------------------------
    IndexSpaceNode::IndexSpaceNode(IndexSpace h, const Domain &d, 
                                   IndexPartNode *par, ColorPoint c,
                                   IndexSpaceKind k, AllocateMode m,
                                   RegionTreeForest *ctx)
      : IndexTreeNode(c, (par == NULL) ? 0 : par->depth+1, ctx),
        handle(h), parent(par), kind(k), mode(m), 
        handle_ready(ApEvent::NO_AP_EVENT), domain_ready(ApEvent::NO_AP_EVENT), 
        domain(d),  allocator(NULL)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    IndexSpaceNode::IndexSpaceNode(IndexSpace h, const Domain &d, ApEvent r, 
                                   IndexPartNode *par, ColorPoint c, 
                                   IndexSpaceKind k, AllocateMode m, 
                                   RegionTreeForest *ctx)
      : IndexTreeNode(c, (par == NULL) ? 0 : par->depth+1, ctx),
        handle(h), parent(par), kind(k), mode(m), 
        handle_ready(ApEvent::NO_AP_EVENT), domain_ready(r), 
        domain(d), allocator(NULL)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    IndexSpaceNode::IndexSpaceNode(IndexSpace h,ApEvent h_ready,ApEvent d_ready,
                                   IndexPartNode *par, ColorPoint c,
                                   IndexSpaceKind k, AllocateMode m,
                                   RegionTreeForest *ctx)
      : IndexTreeNode(c, (par == NULL) ? 0 : par->depth+1, ctx),
        handle(h), parent(par), kind(k), mode(m), handle_ready(h_ready),
        domain_ready(d_ready), allocator(NULL)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    IndexSpaceNode::IndexSpaceNode(const IndexSpaceNode &rhs)
      : IndexTreeNode(), handle(IndexSpace::NO_SPACE), parent(NULL), 
        kind(rhs.kind), mode(rhs.mode), handle_ready(ApEvent::NO_AP_EVENT), 
        domain_ready(ApEvent::NO_AP_EVENT), domain(Domain::NO_DOMAIN),
        allocator(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    IndexSpaceNode::~IndexSpaceNode(void)
    //--------------------------------------------------------------------------
    {
      if (allocator != NULL)
      {
        free(allocator);
        allocator = NULL;
      }
    }

    //--------------------------------------------------------------------------
    IndexSpaceNode& IndexSpaceNode::operator=(const IndexSpaceNode &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void* IndexSpaceNode::operator new(size_t count)
    //--------------------------------------------------------------------------
    {
      return legion_alloc_aligned<IndexSpaceNode,true/*bytes*/>(count);
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::operator delete(void *ptr)
    //--------------------------------------------------------------------------
    {
      free(ptr);
    }

    //--------------------------------------------------------------------------
    bool IndexSpaceNode::is_index_space_node(void) const
    //--------------------------------------------------------------------------
    {
      return true;
    }

#ifdef DEBUG_LEGION
    //--------------------------------------------------------------------------
    IndexSpaceNode* IndexSpaceNode::as_index_space_node(void)
    //--------------------------------------------------------------------------
    {
      return this;
    }

    //--------------------------------------------------------------------------
    IndexPartNode* IndexSpaceNode::as_index_part_node(void)
    //--------------------------------------------------------------------------
    {
      return NULL;
    }
#endif

    //--------------------------------------------------------------------------
    AddressSpaceID IndexSpaceNode::get_owner_space(void) const
    //--------------------------------------------------------------------------
    {
      return get_owner_space(handle, context->runtime);
    }

    //--------------------------------------------------------------------------
    /*static*/ AddressSpaceID IndexSpaceNode::get_owner_space(IndexSpace handle,
                                                              Runtime *rt)
    //--------------------------------------------------------------------------
    {
      return (handle.id % rt->runtime_stride);
    }

    //--------------------------------------------------------------------------
    IndexTreeNode* IndexSpaceNode::get_parent(void) const
    //--------------------------------------------------------------------------
    {
      return parent;
    }

    //--------------------------------------------------------------------------
    size_t IndexSpaceNode::get_num_elmts(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(kind == UNSTRUCTURED_KIND);
#endif
      if (parent != NULL)
        return parent->get_num_elmts();
      const Domain &dom = get_domain_blocking();
      return dom.get_index_space().get_valid_mask().get_num_elmts();
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::send_semantic_request(AddressSpaceID target,
             SemanticTag tag, bool can_fail, bool wait_until, RtUserEvent ready)
    //--------------------------------------------------------------------------
    {
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(tag);
        rez.serialize(can_fail);
        rez.serialize(wait_until);
        rez.serialize(ready);
      }
      context->runtime->send_index_space_semantic_request(target, rez);
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::send_semantic_info(AddressSpaceID target,
                                            SemanticTag tag,
                                            const void *buffer, size_t size,
                                            bool is_mutable)
    //--------------------------------------------------------------------------
    {
      // Package up the message first
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(tag);
        rez.serialize(size);
        rez.serialize(buffer, size);
        rez.serialize(is_mutable);
      }
      context->runtime->send_index_space_semantic_info(target, rez);
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::process_semantic_request(SemanticTag tag,
       AddressSpaceID source, bool can_fail, bool wait_until, RtUserEvent ready)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(get_owner_space() == context->runtime->address_space);
#endif
      RtEvent precondition;
      void *result = NULL;
      size_t size = 0;
      bool is_mutable = false;
      {
        AutoLock n_lock(node_lock);
        // See if we already have the data
        LegionMap<SemanticTag,SemanticInfo>::aligned::iterator finder = 
          semantic_info.find(tag);
        if (finder != semantic_info.end())
        {
          if (finder->second.is_valid())
          {
            result = finder->second.buffer;
            size = finder->second.size;
            is_mutable = finder->second.is_mutable;
          }
          else if (!can_fail && wait_until)
            precondition = finder->second.ready_event;
        }
        else if (!can_fail && wait_until)
        {
          // Don't have it yet, make a condition and hope that one comes
          RtUserEvent ready_event = Runtime::create_rt_user_event();
          precondition = ready_event;
          semantic_info[tag] = SemanticInfo(ready_event);
        }
      }
      if (result == NULL)
      {
        if (can_fail || !wait_until)
          Runtime::trigger_event(ready);
        else
        {
          // Defer this until the semantic condition is ready
          SemanticRequestArgs args;
          args.hlr_id = HLR_INDEX_SPACE_SEMANTIC_INFO_REQ_TASK_ID;
          args.proxy_this = this;
          args.tag = tag;
          args.source = source;
          context->runtime->issue_runtime_meta_task(&args, sizeof(args),
                              HLR_INDEX_SPACE_SEMANTIC_INFO_REQ_TASK_ID,
                              HLR_LATENCY_PRIORITY, NULL/*op*/, precondition);
        }
      }
      else
        send_semantic_info(source, tag, result, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::handle_semantic_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexSpace handle;
      derez.deserialize(handle);
      SemanticTag tag;
      derez.deserialize(tag);
      bool can_fail;
      derez.deserialize(can_fail);
      bool wait_until;
      derez.deserialize(wait_until);
      RtUserEvent ready;
      derez.deserialize(ready);
      IndexSpaceNode *node = forest->get_node(handle);
      node->process_semantic_request(tag, source, can_fail, wait_until, ready);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::handle_semantic_info(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexSpace handle;
      derez.deserialize(handle);
      SemanticTag tag;
      derez.deserialize(tag);
      size_t size;
      derez.deserialize(size);
      const void *buffer = derez.get_current_pointer();
      derez.advance_pointer(size);
      bool is_mutable;
      derez.deserialize(is_mutable);
      forest->attach_semantic_information(handle, tag, source, 
                                          buffer, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    bool IndexSpaceNode::has_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      std::map<ColorPoint,IndexPartNode*>::const_iterator finder = 
        color_map.find(c);
      return ((finder != color_map.end()) && (finder->second != NULL));
    }

    //--------------------------------------------------------------------------
    IndexPartNode* IndexSpaceNode::get_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      // See if we have it locally if not go find it
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<ColorPoint,IndexPartNode*>::const_iterator finder = 
          color_map.find(c);
        if (finder != color_map.end())
        {
#ifdef DEBUG_LEGION
          assert(finder->second != NULL);
#endif
          return finder->second;
        }
      }
      // if we make it here, send a request
      AddressSpaceID owner_space = get_owner_space();
      if (owner_space == context->runtime->address_space)
      {
        switch (c.get_dim())
        {
          case 0:
            {
              log_index.error("Unable to find entry for color %d in "
                              "index space %x.", c.get_index(), handle.id);
              break;
            }
          case 1:
            {
              log_index.error("Unable to find entry for color %d in "
                              "index space %x.", c[0], handle.id);
              break;
            }
          case 2:
            {
              log_index.error("Unable to find entry for color (%d,%d) in "
                              "index space %x.", c[0], c[1], handle.id);
              break;
            }
          case 3:
            {
              log_index.error("Unable to find entry for color (%d,%d,%d) in "
                              "index space %x.", c[0], c[1], c[2], handle.id);
              break;
            }
          default:
            assert(false);
        }
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_PARTITION_COLOR);
      }
      RtUserEvent ready_event = Runtime::create_rt_user_event();
      IndexPartition child_handle = IndexPartition::NO_PART;
      IndexPartition *volatile handle_ptr = &child_handle;
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(c);
        rez.serialize(handle_ptr);
        rez.serialize(ready_event);
      }
      context->runtime->send_index_space_child_request(owner_space, rez);
      ready_event.wait();
      // Stupid volatile-ness
      IndexPartition handle_copy = *handle_ptr;
#ifdef DEBUG_LEGION
      // stupid volatile-ness
      assert(handle_copy.exists());
#endif
      return context->get_node(handle_copy);
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::add_child(IndexPartNode *child)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
#ifdef DEBUG_LEGION
      // Can have a NULL pointer
      assert((color_map.find(child->color) == color_map.end()) ||
             (color_map[child->color] == NULL));
#endif
      color_map[child->color] = child;
      valid_map[child->color] = child;
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::remove_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      valid_map.erase(c);
    }

    //--------------------------------------------------------------------------
    size_t IndexSpaceNode::get_num_children(void) const
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      return valid_map.size();
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::get_children(
                                  std::map<ColorPoint,IndexPartNode*> &children)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclsuve*/);
      children = color_map;
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::get_child_colors(std::set<ColorPoint> &child_colors,
                                          bool only_valid)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      if (only_valid)
      {
        for (std::map<ColorPoint,IndexPartNode*>::const_iterator it = 
              valid_map.begin(); it != valid_map.end(); it++)
          child_colors.insert(it->first);
      }
      else
      {
        for (std::map<ColorPoint,IndexPartNode*>::const_iterator it = 
              color_map.begin(); it != color_map.end(); it++)
          child_colors.insert(it->first);
      }
    }

    //--------------------------------------------------------------------------
    ApEvent IndexSpaceNode::get_domain_precondition(void)
    //--------------------------------------------------------------------------
    {
      if (!handle_ready.has_triggered())
        handle_ready.wait();
      return domain_ready;
    }

    //--------------------------------------------------------------------------
    const Domain& IndexSpaceNode::get_domain_blocking(void)
    //--------------------------------------------------------------------------
    {
      if (!handle_ready.has_triggered())
        handle_ready.wait();
      if (!domain_ready.has_triggered())
        domain_ready.wait();
      return domain;
    }

    //--------------------------------------------------------------------------
    const Domain& IndexSpaceNode::get_domain(ApEvent &precondition)
    //--------------------------------------------------------------------------
    {
      if (!handle_ready.has_triggered())
        handle_ready.wait();
      precondition = domain_ready;
      return domain;
    }

    //--------------------------------------------------------------------------
    const Domain& IndexSpaceNode::get_domain_no_wait(void)
    //--------------------------------------------------------------------------
    {
      // We still need to wait for the handle to be valid
      if (!handle_ready.has_triggered())
        handle_ready.wait();
      return domain;
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::set_domain(const Domain &dom)
    //--------------------------------------------------------------------------
    {
      domain = dom;
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::get_domains_blocking(std::vector<Domain> &domains) 
    //--------------------------------------------------------------------------
    {
      if (!handle_ready.has_triggered())
        handle_ready.wait();
      if (!domain_ready.has_triggered())
        domain_ready.wait();
      if (has_component_domains())
      {
        domains.insert(domains.end(), 
                       component_domains.begin(), component_domains.end());
      }
      else
        domains.push_back(domain);
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::get_domains(std::vector<Domain> &domains, 
                                     ApEvent &precondition)
    //--------------------------------------------------------------------------
    {
      if (!handle_ready.has_triggered())
        handle_ready.wait();
      precondition = domain_ready;
      if (has_component_domains())
      {
        domains.insert(domains.end(), 
                       component_domains.begin(), component_domains.end());
      }
      else
        domains.push_back(domain);
    }

    //--------------------------------------------------------------------------
    size_t IndexSpaceNode::get_domain_volume(bool app_query)
    //--------------------------------------------------------------------------
    {
      if (!handle_ready.has_triggered())
        handle_ready.wait();
      if (!domain_ready.has_triggered())
        domain_ready.wait();
      if (domain.get_dim() == 0)
      {
        const Realm::ElementMask &mask = 
                                  domain.get_index_space().get_valid_mask();
        return mask.get_num_elmts();
      }
      else
        return domain.get_volume();
    }

    //--------------------------------------------------------------------------
    bool IndexSpaceNode::are_disjoint(const ColorPoint &c1, 
                                      const ColorPoint &c2)
    //--------------------------------------------------------------------------
    {
      // Quick out
      if (c1 == c2)
        return false;
      // Do the test with read-only mode first
      RtEvent ready;
      bool issue_dynamic_test = false;
      std::pair<ColorPoint,ColorPoint> key(c1,c2);
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        if (disjoint_subsets.find(key) != disjoint_subsets.end())
          return true;
        else if (aliased_subsets.find(key) != aliased_subsets.end())
          return false;
        else
        {
          std::map<std::pair<ColorPoint,ColorPoint>,RtEvent>::const_iterator
            finder = pending_tests.find(key);
          if (finder != pending_tests.end())
            ready = finder->second;
          else
          {
            if (Runtime::dynamic_independence_tests)
              issue_dynamic_test = true;
            else
            {
              aliased_subsets.insert(key);
              aliased_subsets.insert(std::pair<ColorPoint,ColorPoint>(c2,c1));
              return false;
            }
          }
        }
      }
      if (issue_dynamic_test)
      {
        IndexPartNode *left = get_child(c1);
        IndexPartNode *right = get_child(c2);
        std::set<ApEvent> preconditions; 
        left->get_subspace_domain_preconditions(preconditions);
        right->get_subspace_domain_preconditions(preconditions);
        AutoLock n_lock(node_lock);
        // Test again to make sure we didn't lose the race
        std::map<std::pair<ColorPoint,ColorPoint>,RtEvent>::const_iterator
          finder = pending_tests.find(key);
        if (finder == pending_tests.end())
        {
          DynamicIndependenceArgs args;
          args.hlr_id = HLR_PART_INDEPENDENCE_TASK_ID;
          args.parent = this;
          args.left = left;
          args.right = right;
          // Get the preconditions for domains 
          RtEvent pre = Runtime::protect_merge_events(preconditions);
          ready = context->runtime->issue_runtime_meta_task(&args, sizeof(args),
                                      HLR_PART_INDEPENDENCE_TASK_ID, 
                                      HLR_LATENCY_PRIORITY, NULL, pre);
          pending_tests[key] = ready;
          pending_tests[std::pair<ColorPoint,ColorPoint>(c2,c1)] = ready;
        }
        else
          ready = finder->second;
      }
      // Wait for the ready event and then get the result
      ready.wait();
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      if (disjoint_subsets.find(key) != disjoint_subsets.end())
        return true;
      else
        return false;
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::record_disjointness(bool disjoint, 
                                     const ColorPoint &c1, const ColorPoint &c2)
    //--------------------------------------------------------------------------
    {
      if (c1 == c2)
        return;
      AutoLock n_lock(node_lock);
#ifdef DEBUG_LEGION
      assert(color_map.find(c1) != color_map.end());
      assert(color_map.find(c2) != color_map.end());
#endif
      if (disjoint)
      {
        disjoint_subsets.insert(std::pair<ColorPoint,ColorPoint>(c1,c2));
        disjoint_subsets.insert(std::pair<ColorPoint,ColorPoint>(c2,c1));
      }
      else
      {
        aliased_subsets.insert(std::pair<ColorPoint,ColorPoint>(c1,c2));
        aliased_subsets.insert(std::pair<ColorPoint,ColorPoint>(c2,c1));
      }
      pending_tests.erase(std::pair<ColorPoint,ColorPoint>(c1,c2));
      pending_tests.erase(std::pair<ColorPoint,ColorPoint>(c2,c1));
    }

    //--------------------------------------------------------------------------
    Color IndexSpaceNode::generate_color(void)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      Color result;
      if (!color_map.empty())
      {
        unsigned stride = context->runtime->get_color_modulus();
        std::map<ColorPoint,IndexPartNode*>::const_reverse_iterator rlast = 
                                                        color_map.rbegin();
#ifdef DEBUG_LEGION
        assert(rlast->first.get_dim() == 1);
#endif
        // We know all colors for index spaces are 0-D
        Color new_color =
          (rlast->first[0] + (stride - 1)) / stride * stride +
          context->runtime->get_start_color();
        if (new_color == (Color)rlast->first[0]) new_color += stride;
        result = new_color;
      }
      else
        result = context->runtime->get_start_color();
      ColorPoint color(result);
#ifdef DEBUG_LEGION
      assert(color_map.find(color) == color_map.end());
#endif
      // We have to put ourselves in the map to be sound for other parallel
      // allocations of colors which may come later
      color_map[color] = NULL; /* just put in a NULL pointer for now */
      return result;
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::get_colors(std::set<ColorPoint> &colors)
    //--------------------------------------------------------------------------
    {
      // If we're not the owner, we need to request an up to date set of colors
      // since it can change arbitrarily
      AddressSpaceID owner_space = get_owner_space();
      if (owner_space != context->runtime->address_space)
      {
        RtUserEvent ready_event = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(handle);
          rez.serialize(&colors);
          rez.serialize(ready_event); 
        }
        context->runtime->send_index_space_colors_request(owner_space, rez);
        ready_event.wait();
      }
      else
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        for (std::map<ColorPoint,IndexPartNode*>::const_iterator it = 
              valid_map.begin(); it != valid_map.end(); it++)
        {
          // Can be NULL in some cases of parallel partitioning
          if (it->second != NULL)
            colors.insert(it->first);
        }
      }
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::add_instance(RegionNode *inst)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
#ifdef DEBUG_LEGION
      assert(logical_nodes.find(inst) == logical_nodes.end());
#endif
      logical_nodes.insert(inst);
    }

    //--------------------------------------------------------------------------
    bool IndexSpaceNode::has_instance(RegionTreeID tid)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      for (std::set<RegionNode*>::const_iterator it = logical_nodes.begin();
            it != logical_nodes.end(); it++)
      {
        if ((*it)->handle.get_tree_id() == tid)
          return true;
      }
      return false;
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::add_creation_source(AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      creation_set.add(source);
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::DestructionFunctor::apply(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      runtime->send_index_space_destruction(handle, target);
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::destroy_node(AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      // If we've already been destroyed then we are done
      if (destroyed)
        return;
      AutoLock n_lock(node_lock);
      if (!destroyed)
      {
        destroyed = true;
        if (!creation_set.empty())
        {
          DestructionFunctor functor(handle, context->runtime);
          creation_set.map(functor);
        }
        for (std::set<RegionNode*>::const_iterator it = logical_nodes.begin();
              it != logical_nodes.end(); it++)
        {
          (*it)->destroy_node(source);
        }
      }
    }

    //--------------------------------------------------------------------------
    bool IndexSpaceNode::has_component_domains(void) const
    //--------------------------------------------------------------------------
    {
      return !component_domains.empty();
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::update_component_domains(const std::set<Domain> &doms)
    //--------------------------------------------------------------------------
    {
      component_domains.insert(doms.begin(), doms.end());
    }

    //--------------------------------------------------------------------------
    const std::set<Domain>& IndexSpaceNode::get_component_domains_blocking(
                                                                     void) const
    //--------------------------------------------------------------------------
    {
      if (!handle_ready.has_triggered())
        handle_ready.wait();
      if (!domain_ready.has_triggered())
        domain_ready.wait();
      return component_domains;
    }

    //--------------------------------------------------------------------------
    const std::set<Domain>& IndexSpaceNode::get_component_domains(
                                                           ApEvent &ready) const
    //--------------------------------------------------------------------------
    {
      if (!handle_ready.has_triggered())
        handle_ready.wait();
      ready = domain_ready;
      return component_domains;
    }

    //--------------------------------------------------------------------------
    bool IndexSpaceNode::intersects_with(IndexSpaceNode *other, bool compute)
    //--------------------------------------------------------------------------
    {
      if (other == this)
        return true;
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        // Only return the value if we either didn't want to compute
        // or we already have valid intersections
        if ((finder != intersections.end()) && 
            (!compute || finder->second.intersections_valid))
          return finder->second.has_intersects;
      }
      std::set<Domain> intersect;
      bool result;
      if (component_domains.empty())
      { 
        if (other->has_component_domains())
          result = compute_intersections(
                                   other->get_component_domains_blocking(),
                                   get_domain_blocking(), intersect, compute);
        else
        {
          Domain inter;
          result = compute_intersection(get_domain_blocking(), 
                                        other->get_domain_blocking(),
                                        inter, compute);
          if (result)
            intersect.insert(inter);
        }
      }
      else
      {
        if (other->has_component_domains())
          result = compute_intersections(component_domains,
                  other->get_component_domains_blocking(), intersect, compute);
        else
          result = compute_intersections(component_domains,
                                         other->get_domain_blocking(), 
                                         intersect, compute); 
      }
      AutoLock n_lock(node_lock);
      if (result)
      {
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        // Check to make sure we didn't lose the race
        if ((finder == intersections.end()) || 
            (compute && !finder->second.intersections_valid))
        {
          if (compute)
            intersections[other] = IntersectInfo(intersect);
          else
            intersections[other] = IntersectInfo(true/*result*/);
        }
      }
      else
        intersections[other] = IntersectInfo(false/*result*/);
      return result;
    }

    //--------------------------------------------------------------------------
    bool IndexSpaceNode::intersects_with(IndexPartNode *other, bool compute)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        // Only return the value if we know we are valid and we didn't
        // want to compute anything or we already did compute it
        if ((finder != intersections.end()) &&
            (!compute || finder->second.intersections_valid))
          return finder->second.has_intersects;
      }
      // Build up the set of domains for the partition
      std::set<Domain> other_domains, intersect;
      other->get_subspace_domains(other_domains);
      bool result;
      if (component_domains.empty())
      {
        result = compute_intersections(other_domains, get_domain_blocking(), 
                                       intersect, compute);
      }
      else
      {
        result = compute_intersections(component_domains, other_domains,
                                       intersect, compute);
      }
      AutoLock n_lock(node_lock);
      if (result)
      {
        // Check to make sure we didn't lose the race
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder == intersections.end()) ||
            (compute && !finder->second.intersections_valid))
        {
          if (compute)
            intersections[other] = IntersectInfo(intersect);
          else
            intersections[other] = IntersectInfo(true/*result*/);
        }
      }
      else
        intersections[other] = IntersectInfo(false/*result*/);
      return result;
    }

    //--------------------------------------------------------------------------
    const std::set<Domain>& IndexSpaceNode::get_intersection_domains(
                                                          IndexSpaceNode *other)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder != intersections.end()) &&
            finder->second.intersections_valid)
          return finder->second.intersections;
      }
      std::set<Domain> intersect;
      bool result;
      if (component_domains.empty())
      { 
        if (other->has_component_domains())
          result = compute_intersections(
                    other->get_component_domains_blocking(),
                    get_domain_blocking(), intersect, true/*compute*/);
        else
        {
          Domain inter;
          result = compute_intersection(get_domain_blocking(), 
                                        other->get_domain_blocking(), 
                                        inter, true/*compute*/);
          if (result)
            intersect.insert(inter);
        }
      }
      else
      {
        if (other->has_component_domains())
          result = compute_intersections(component_domains,
                  other->get_component_domains_blocking(), 
                  intersect, true/*compute*/);
        else
          result = compute_intersections(component_domains,
                                         other->get_domain_blocking(), 
                                         intersect, true/*compute*/); 
      }
      AutoLock n_lock(node_lock);
      if (result)
      {
        // Check again to make sure we didn't lose the race
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder == intersections.end()) ||
            !finder->second.intersections_valid)
          intersections[other] = IntersectInfo(intersect);
      }
      else
        intersections[other] = IntersectInfo(false/*result*/);
      return intersections[other].intersections;
    }

    //--------------------------------------------------------------------------
    const std::set<Domain>& IndexSpaceNode::get_intersection_domains(
                                                           IndexPartNode *other)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder != intersections.end()) &&
            finder->second.intersections_valid)
          return finder->second.intersections;
      }
      // Build up the set of domains for the partition
      std::set<Domain> other_domains, intersect;
      other->get_subspace_domains(other_domains);
      bool result;
      if (component_domains.empty())
      {
        result = compute_intersections(other_domains, get_domain_blocking(), 
                                       intersect, true/*compute*/);
      }
      else
      {
        result = compute_intersections(component_domains, other_domains,
                                       intersect, true/*compute*/);
      }
      AutoLock n_lock(node_lock);
      if (result)
      {
        // Check again to make sure we didn't lose the race
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder == intersections.end()) ||
            !finder->second.intersections_valid)
          intersections[other] = IntersectInfo(intersect);
      }
      else
        intersections[other] = IntersectInfo(false/*result*/);
      return intersections[other].intersections;
    }

    //--------------------------------------------------------------------------
    bool IndexSpaceNode::dominates(IndexSpaceNode *other)
    //--------------------------------------------------------------------------
    {
      if (other == this)
        return true;
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,bool>::const_iterator finder = 
          dominators.find(other);
        if (finder != dominators.end())
          return finder->second;
      }
      bool result;
      if (component_domains.empty())
      {
        if (other->has_component_domains())
        {
          std::set<Domain> local;
          local.insert(get_domain_blocking());
          result = compute_dominates(local, 
                                     other->get_component_domains_blocking());
        }
        else
        {
          std::set<Domain> left, right;
          left.insert(get_domain_blocking());
          right.insert(other->get_domain_blocking());
          result = compute_dominates(left, right);
        }
      }
      else
      {
        if (other->has_component_domains())
          result = compute_dominates(component_domains,   
                                     other->get_component_domains_blocking()); 
        else
        {
          std::set<Domain> other_doms;
          other_doms.insert(other->get_domain_blocking());
          result = compute_dominates(component_domains, other_doms);
        }
      }
      AutoLock n_lock(node_lock);
      dominators[other] = result;
      return result;
    }

    //--------------------------------------------------------------------------
    bool IndexSpaceNode::dominates(IndexPartNode *other)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,bool>::const_iterator finder = 
          dominators.find(other);
        if (finder != dominators.end())
          return finder->second;
      }
      bool result;
      std::set<Domain> other_doms;
      other->get_subspace_domains(other_doms);
      if (component_domains.empty())
      {
        std::set<Domain> local;
        local.insert(get_domain_blocking());
        result = compute_dominates(local, other_doms);
      }
      else
        result = compute_dominates(component_domains, other_doms);
      AutoLock n_lock(node_lock);
      dominators[other] = result;
      return result;
    }

    //--------------------------------------------------------------------------
    ApEvent IndexSpaceNode::create_subspaces_by_field(
                        const std::vector<FieldDataDescriptor> &field_data,
                        std::map<DomainPoint, Realm::IndexSpace> &subspaces,
                        bool mutable_results, ApEvent precondition)
    //--------------------------------------------------------------------------
    {
      ApEvent dom_precondition;
      const Domain &dom = get_domain(dom_precondition);
      return ApEvent(dom.get_index_space().create_subspaces_by_field(field_data,
                                     subspaces, mutable_results, 
                                     Runtime::merge_events(precondition,
                                                           dom_precondition)));
    }

    //--------------------------------------------------------------------------
    ApEvent IndexSpaceNode::create_subspaces_by_image(
                const std::vector<FieldDataDescriptor> &field_data,
                std::map<Realm::IndexSpace, Realm::IndexSpace> &subspaces,
                bool mutable_results, ApEvent precondition)
    //--------------------------------------------------------------------------
    {
      ApEvent dom_precondition;
      const Domain &dom = get_domain(dom_precondition);
      return ApEvent(dom.get_index_space().create_subspaces_by_image(field_data,
                                     subspaces, mutable_results, 
                                     Runtime::merge_events(precondition,
                                                          dom_precondition)));
    }

    //--------------------------------------------------------------------------
    ApEvent IndexSpaceNode::create_subspaces_by_preimage(
                const std::vector<FieldDataDescriptor> &field_data,
                std::map<Realm::IndexSpace, Realm::IndexSpace> &subspaces,
                bool mutable_results, ApEvent precondition)
    //--------------------------------------------------------------------------
    {
      ApEvent dom_precondition;
      const Domain &dom = get_domain(dom_precondition);
      return ApEvent(dom.get_index_space().create_subspaces_by_preimage(
                                     field_data, subspaces, mutable_results, 
                                     Runtime::merge_events(precondition,
                                                           dom_precondition)));
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::handle_disjointness_test(
              IndexSpaceNode *parent, IndexPartNode *left, IndexPartNode *right)
    //--------------------------------------------------------------------------
    {
      std::map<ColorPoint,IndexSpaceNode*> left_spaces, right_spaces;    
      left->get_children(left_spaces);
      right->get_children(right_spaces);
      bool disjoint = true;
      for (std::map<ColorPoint,IndexSpaceNode*>::const_iterator lit = 
            left_spaces.begin(); disjoint && (lit != left_spaces.end()); lit++)
      {
        for (std::map<ColorPoint,IndexSpaceNode*>::const_iterator rit = 
              right_spaces.begin(); disjoint && 
              (rit != right_spaces.end()); rit++)
        {
          if (!RegionTreeForest::are_disjoint(lit->second, rit->second))
            disjoint = false;
        }
      }
      parent->record_disjointness(disjoint, left->color, right->color);
    }

    //--------------------------------------------------------------------------
    void IndexSpaceNode::send_node(AddressSpaceID target, bool up, bool down)
    //--------------------------------------------------------------------------
    {
      // Go up first so we know those nodes will be there
      if (up && (parent != NULL))
        parent->send_node(target, true/*up*/, false/*down*/);
      // Check to see if we need to wait for the handle event to be ready
      if (!handle_ready.has_triggered())
          handle_ready.wait();
      // Check to see if our creation set includes the target
      std::map<ColorPoint,IndexPartNode*> valid_copy;
      {
        AutoLock n_lock(node_lock);
        if (!creation_set.contains(target))
        {
          Serializer rez;
          {
            RezCheck z(rez);
            rez.serialize(handle);
            rez.serialize(domain);
            rez.serialize(domain_ready);
            rez.serialize(kind);
            rez.serialize(mode);
            if (parent != NULL)
              rez.serialize(parent->handle);
            else
              rez.serialize(IndexPartition::NO_PART);
            rez.serialize(color);
            rez.serialize(component_domains.size());
            for (std::set<Domain>::const_iterator it = 
                  component_domains.begin(); it != 
                  component_domains.end(); it++)
            {
              rez.serialize(*it);
            }
            rez.serialize<size_t>(semantic_info.size());
            for (LegionMap<SemanticTag,SemanticInfo>::aligned::iterator it = 
                  semantic_info.begin(); it != semantic_info.end(); it++)
            {
              rez.serialize(it->first);
              rez.serialize(it->second.size);
              rez.serialize(it->second.buffer, it->second.size);
              rez.serialize(it->second.is_mutable);
            }
          }
          context->runtime->send_index_space_node(target, rez); 
          creation_set.add(target);
        }
        // Also check to see if we need to go down
        if (down && child_creation.contains(target))
          down = false;
        if (destroyed)
          // Now we need to send a destruction
          context->runtime->send_index_space_destruction(handle, target);
        // If we need to go down, make a copy of the valid children
        if (down)
          valid_copy = valid_map;
      }
      if (down)
      {
        for (std::map<ColorPoint,IndexPartNode*>::const_iterator it = 
              valid_copy.begin(); it != valid_copy.end(); it++)
        {
          it->second->send_node(target, false/*up*/, true/*down*/);
        }
        // If we sent all our children, then we can record it
        AutoLock n_lock(node_lock);
        child_creation.add(target);
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::handle_node_creation(
        RegionTreeForest *context, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexSpace handle;
      derez.deserialize(handle);
      Domain domain;
      derez.deserialize(domain);
      ApEvent ready_event;
      derez.deserialize(ready_event);
      IndexSpaceKind kind;
      derez.deserialize(kind);
      AllocateMode mode;
      derez.deserialize(mode);
      IndexPartition parent;
      derez.deserialize(parent);
      ColorPoint color;
      derez.deserialize(color);
      size_t components;
      derez.deserialize(components);
      std::set<Domain> component_domains;
      for (unsigned idx = 0; idx < components; idx++)
      {
        Domain component;
        derez.deserialize(component);
        component_domains.insert(component);
      }
      IndexPartNode *parent_node = NULL;
      if (parent != IndexPartition::NO_PART)
      {
        parent_node = context->get_node(parent);
#ifdef DEBUG_LEGION
        assert(parent_node != NULL);
#endif
      }
      IndexSpaceNode *node = 
                  context->create_node(handle, domain, ready_event, 
                                       parent_node, color,
                                       kind, mode);
#ifdef DEBUG_LEGION
      assert(node != NULL);
#endif
      node->add_creation_source(source);
      if (components > 0)
        node->update_component_domains(component_domains);
      size_t num_semantic;
      derez.deserialize(num_semantic);
      for (unsigned idx = 0; idx < num_semantic; idx++)
      {
        SemanticTag tag;
        derez.deserialize(tag);
        size_t buffer_size;
        derez.deserialize(buffer_size);
        const void *buffer = derez.get_current_pointer();
        derez.advance_pointer(buffer_size);
        bool is_mutable;
        derez.deserialize(is_mutable);
        node->attach_semantic_information(tag, source, 
                                          buffer, buffer_size, is_mutable);
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::handle_node_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      IndexSpace handle;
      derez.deserialize(handle);
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      IndexSpaceNode *target = forest->get_node(handle);
      target->send_node(source, true/*up*/, false/*down*/);
      // Then send back the flush
      Serializer rez;
      rez.serialize(to_trigger);
      forest->runtime->send_index_space_return(source, rez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::handle_node_return(Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      Runtime::trigger_event(to_trigger);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::handle_node_child_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexSpace handle;
      derez.deserialize(handle);
      ColorPoint child_color;
      derez.deserialize(child_color);
      IndexPartNode *target;
      derez.deserialize(target);
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      IndexSpaceNode *parent = forest->get_node(handle);
      IndexPartNode *child = parent->get_child(child_color);
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(child->handle);
        rez.serialize(target);
        rez.serialize(to_trigger);
      }
      forest->runtime->send_index_space_child_response(source, rez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::handle_node_child_response(
                                                            Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexPartition handle;
      derez.deserialize(handle);
      IndexPartition *target;
      derez.deserialize(target);
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      (*target) = handle;
      Runtime::trigger_event(to_trigger);
    }

    //--------------------------------------------------------------------------
    IndexSpaceAllocator* IndexSpaceNode::get_allocator(void)
    //--------------------------------------------------------------------------
    {
      if (kind != UNSTRUCTURED_KIND)
      {
        log_run.error("Illegal request for an allocator on a structured "
                      "index space! Only unstructured index spaces are "
                      "permitted to have allocators.");
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_ILLEGAL_ALLOCATOR_REQUEST);
      }
      if (allocator == NULL)
      {
        AutoLock n_lock(node_lock);
        if (allocator == NULL)
        {
          allocator = (IndexSpaceAllocator*)malloc(sizeof(IndexSpaceAllocator));
          const Domain &dom = get_domain_blocking();
          *allocator = dom.get_index_space().create_allocator();
        }
      }
      return allocator;
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::log_index_space_domain(IndexSpace handle,
                                                           const Domain &dom)
    //--------------------------------------------------------------------------
    {
      switch (dom.get_dim())
      {
        case 0:
          {
            const Realm::ElementMask &mask = 
              dom.get_index_space().get_valid_mask();
            Realm::ElementMask::Enumerator *enumerator = 
              mask.enumerate_enabled();
            coord_t next;
            size_t length;
            bool empty = true;
            while (enumerator->get_next(next, length))
            {
              long long int begin = next;
              if (length > 1)
              {
                // inclusive so need -1
                coord_t end = next + length - 1;
                LegionSpy::log_index_space_rect<1>(handle.id, &begin, &end);
              }
              else
                LegionSpy::log_index_space_point<1>(handle.id, &begin);
              empty = false;
            }
            delete enumerator;
            if (empty)
              LegionSpy::log_empty_index_space(handle.id);
            break;
          }
        case 1:
          {
            Rect<1> rect = dom.get_rect<1>();
            if (rect.volume() > 0)
              LegionSpy::log_index_space_rect<1>(handle.id,rect.lo.x,rect.hi.x);
            else
              LegionSpy::log_empty_index_space(handle.id);
            break;
          }
        case 2:
          {
            Rect<2> rect = dom.get_rect<2>();
            if (rect.volume() > 0)
              LegionSpy::log_index_space_rect<2>(handle.id,rect.lo.x,rect.hi.x);
            else
              LegionSpy::log_empty_index_space(handle.id);
            break;
          }
        case 3:
          {
            Rect<3> rect = dom.get_rect<3>();
            if (rect.volume() > 0)
              LegionSpy::log_index_space_rect<3>(handle.id,rect.lo.x,rect.hi.x);
            else
              LegionSpy::log_empty_index_space(handle.id);
            break;
          }
        default:
          assert(false);
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::handle_colors_request(
          RegionTreeForest *context, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexSpace handle;
      derez.deserialize(handle);
      std::set<ColorPoint> *target;
      derez.deserialize(target);
      RtUserEvent ready;
      derez.deserialize(ready);
      IndexSpaceNode *node = context->get_node(handle);
      std::set<ColorPoint> results;
      node->get_colors(results);
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(target);
        rez.serialize<size_t>(results.size());
        for (std::set<ColorPoint>::const_iterator it = results.begin();
              it != results.end(); it++)
          it->serialize(rez);
        rez.serialize(ready);
      }
      context->runtime->send_index_space_colors_response(source, rez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexSpaceNode::handle_colors_response(Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      std::set<ColorPoint> *target;
      derez.deserialize(target);
      size_t num_colors;
      derez.deserialize(num_colors);
      for (unsigned idx = 0; idx < num_colors; idx++)
      {
        ColorPoint cp;
        cp.deserialize(derez);
        target->insert(cp);
      }
      RtUserEvent ready;
      derez.deserialize(ready);
      Runtime::trigger_event(ready);
    }

    /////////////////////////////////////////////////////////////
    // Index Partition Node 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    IndexPartNode::IndexPartNode(IndexPartition p, IndexSpaceNode *par,
                                 ColorPoint c, Domain cspace,
                                 bool dis, AllocateMode m,
                                 RegionTreeForest *ctx)
      : IndexTreeNode(c, par->depth+1, ctx), handle(p), color_space(cspace),
        mode(m), parent(par), disjoint(dis), 
        disjoint_ready(RtEvent::NO_RT_EVENT), has_complete(false)
    //--------------------------------------------------------------------------
    { 
    }

    //--------------------------------------------------------------------------
    IndexPartNode::IndexPartNode(IndexPartition p, IndexSpaceNode *par,
                                 ColorPoint c, Domain cspace,
                                 RtEvent ready, AllocateMode m,
                                 RegionTreeForest *ctx)
      : IndexTreeNode(c, par->depth+1, ctx), handle(p), color_space(cspace),
        mode(m), parent(par), disjoint(false), disjoint_ready(ready), 
        has_complete(false)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    IndexPartNode::IndexPartNode(const IndexPartNode &rhs)
      : IndexTreeNode(), handle(IndexPartition::NO_PART), 
        color_space(Domain::NO_DOMAIN), mode(NO_MEMORY), 
        parent(NULL), disjoint(false), has_complete(false)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    IndexPartNode::~IndexPartNode(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    IndexPartNode& IndexPartNode::operator=(const IndexPartNode &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void* IndexPartNode::operator new(size_t count)
    //--------------------------------------------------------------------------
    {
      return legion_alloc_aligned<IndexPartNode,true/*bytes*/>(count);
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::operator delete(void *ptr)
    //--------------------------------------------------------------------------
    {
      free(ptr);
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::is_index_space_node(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

#ifdef DEBUG_LEGION
    //--------------------------------------------------------------------------
    IndexSpaceNode* IndexPartNode::as_index_space_node(void)
    //--------------------------------------------------------------------------
    {
      return NULL;
    }

    //--------------------------------------------------------------------------
    IndexPartNode* IndexPartNode::as_index_part_node(void)
    //--------------------------------------------------------------------------
    {
      return this;
    }
#endif

    //--------------------------------------------------------------------------
    AddressSpaceID IndexPartNode::get_owner_space(void) const
    //--------------------------------------------------------------------------
    {
      return get_owner_space(handle, context->runtime);
    }

    //--------------------------------------------------------------------------
    /*static*/ AddressSpaceID IndexPartNode::get_owner_space(
                                          IndexPartition part, Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      return (part.id % runtime->runtime_stride);
    }

    //--------------------------------------------------------------------------
    IndexTreeNode* IndexPartNode::get_parent(void) const
    //--------------------------------------------------------------------------
    {
      return parent;
    }

    //--------------------------------------------------------------------------
    size_t IndexPartNode::get_num_elmts(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(parent != NULL);
#endif
      return parent->get_num_elmts();
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::send_semantic_request(AddressSpaceID target, 
             SemanticTag tag, bool can_fail, bool wait_until, RtUserEvent ready)
    //--------------------------------------------------------------------------
    {
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(tag);
        rez.serialize(can_fail);
        rez.serialize(wait_until);
        rez.serialize(ready);
      }
      context->runtime->send_index_partition_semantic_request(target, rez);
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::send_semantic_info(AddressSpaceID target, 
                                           SemanticTag tag, const void *buffer,
                                           size_t size, bool is_mutable)
    //--------------------------------------------------------------------------
    {
      // Package up the message first
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(tag);
        rez.serialize(size);
        rez.serialize(buffer, size);
        rez.serialize(is_mutable);
      }
      context->runtime->send_index_partition_semantic_info(target, rez);
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::process_semantic_request(SemanticTag tag, 
       AddressSpaceID source, bool can_fail, bool wait_until, RtUserEvent ready)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(get_owner_space() == context->runtime->address_space);
#endif
      RtEvent precondition;
      void *result = NULL;
      size_t size = 0;
      bool is_mutable = false;
      {
        AutoLock n_lock(node_lock);
        // See if we already have the data
        LegionMap<SemanticTag,SemanticInfo>::aligned::iterator finder = 
          semantic_info.find(tag);
        if (finder != semantic_info.end())
        {
          if (finder->second.is_valid())
          {
            result = finder->second.buffer;
            size = finder->second.size;
            is_mutable = finder->second.is_mutable;
          }
          else if (!can_fail && wait_until)
            precondition = finder->second.ready_event;
        }
        else if (!can_fail && wait_until)
        {
          // Don't have it yet, make a condition and hope that one comes
          RtUserEvent ready_event = Runtime::create_rt_user_event();
          precondition = ready_event;
          semantic_info[tag] = SemanticInfo(ready_event);
        }
      }
      if (result == NULL)
      {
        if (can_fail || !wait_until)
          Runtime::trigger_event(ready);
        else
        {
          // Defer this until the semantic condition is ready
          SemanticRequestArgs args;
          args.hlr_id = HLR_INDEX_PART_SEMANTIC_INFO_REQ_TASK_ID;
          args.proxy_this = this;
          args.tag = tag;
          args.source = source;
          context->runtime->issue_runtime_meta_task(&args, sizeof(args),
                              HLR_INDEX_PART_SEMANTIC_INFO_REQ_TASK_ID,
                              HLR_LATENCY_PRIORITY, NULL/*op*/, precondition);
        }
      }
      else
        send_semantic_info(source, tag, result, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexPartNode::handle_semantic_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexPartition handle;
      derez.deserialize(handle);
      SemanticTag tag;
      derez.deserialize(tag);
      bool can_fail;
      derez.deserialize(can_fail);
      bool wait_until;
      derez.deserialize(wait_until);
      RtUserEvent ready;
      derez.deserialize(ready);
      IndexPartNode *node = forest->get_node(handle);
      node->process_semantic_request(tag, source, can_fail, wait_until, ready);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexPartNode::handle_semantic_info(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexPartition handle;
      derez.deserialize(handle);
      SemanticTag tag;
      derez.deserialize(tag);
      size_t size;
      derez.deserialize(size);
      const void *buffer = derez.get_current_pointer();
      derez.advance_pointer(size);
      bool is_mutable;
      derez.deserialize(is_mutable);
      forest->attach_semantic_information(handle, tag, source, 
                                          buffer, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::has_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      return (color_map.find(c) != color_map.end());
    }

    //--------------------------------------------------------------------------
    IndexSpaceNode* IndexPartNode::get_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      // First check to see if we can find it
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/); 
        std::map<ColorPoint,IndexSpaceNode*>::const_iterator finder = 
          color_map.find(c);
        if (finder != color_map.end())
          return finder->second;
      }
#ifdef DEBUG_LEGION
      if (!color_space.contains(c.get_point()))
      {
        switch (c.get_dim())
        {
          case 0:
            {
              log_index.error("Invalid request for color %d in "
                              "index partition %x.", c.get_index(), handle.id);
              break;
            }
          case 1:
            {
              log_index.error("Invalid request for color %d in "
                              "index partition %x.", c[0], handle.id);
              break;
            }
          case 2:
            {
              log_index.error("Invalid request for color (%d,%d) in "
                              "index partition %x.", c[0], c[1], handle.id);
              break;
            }
          case 3:
            {
              log_index.error("Invalid request for color (%d,%d,%d) in "
                              "index partition %x.", 
                              c[0], c[1], c[2], handle.id);
              break;
            }
          default:
            assert(false);
        }
        assert(false);
        exit(ERROR_INVALID_INDEX_SPACE_COLOR);
      }
#endif
      AddressSpaceID owner_space = get_owner_space();
      AddressSpaceID local_space = context->runtime->address_space;
      // If we own the index partition, create a new subspace here
      if (owner_space == local_space)
      {
        IndexSpace is(context->runtime->get_unique_index_space_id(),
                      handle.get_tree_id());

        if (Runtime::legion_spy_enabled)
        {
          LegionSpy::log_index_subspace(handle.id, is.id, c.get_point());
          LegionSpy::log_empty_index_space(is.id);
        }

        if (parent->kind == UNSTRUCTURED_KIND)
        {
          // Make a new sub-index space first based on the 
          // parent. Determine if it is allocable based on the
          // properties of this partition object.
          const Domain &parent_dom = parent->get_domain_no_wait();
          Realm::IndexSpace parent_space = parent_dom.get_index_space();
          Realm::ElementMask new_mask(get_num_elmts());
          Realm::IndexSpace new_space =  
            Realm::IndexSpace::create_index_space(parent_space, new_mask,
                                                     (mode & ALLOCABLE));
          IndexSpaceNode *result = context->create_node(is, Domain(new_space),
              this, c, UNSTRUCTURED_KIND, mode);
          return result;
        }
        else
        {
          // Easy case just make an empty domain and use that
          Domain empty = Domain::NO_DOMAIN;
          IndexSpaceNode *result = context->create_node(is, empty,
              this, c, DENSE_ARRAY_KIND, parent->mode);
          return result;
        }
      }
      // Otherwise, request a child node from the owner node
      else
      {
        IndexSpace child_handle = IndexSpace::NO_SPACE;
        IndexSpace *volatile handle_ptr = &child_handle;
        RtUserEvent ready_event = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(handle);
          rez.serialize(c);
          rez.serialize(handle_ptr);
          rez.serialize(ready_event);
        }
        context->runtime->send_index_partition_child_request(owner_space, rez);
        ready_event.wait();
        IndexSpace copy_handle = *handle_ptr;
#ifdef DEBUG_LEGION
        assert(copy_handle.exists());
#endif
        return context->get_node(copy_handle);
      }
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::add_child(IndexSpaceNode *child)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
#ifdef DEBUG_LEGION
      assert(color_map.find(child->color) == color_map.end());
#endif
      color_map[child->color] = child;
      valid_map[child->color] = child;
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::remove_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      valid_map.erase(c);
      // Mark that any completeness computations we've done are no longer valid
      has_complete = false;
    }

    //--------------------------------------------------------------------------
    size_t IndexPartNode::get_num_children(void) const
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      return valid_map.size();
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::get_children(
                                 std::map<ColorPoint,IndexSpaceNode*> &children)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      children = color_map;
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::compute_disjointness(RtUserEvent ready_event)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(disjoint_ready.exists() && !disjoint_ready.has_triggered());
      assert(ready_event == disjoint_ready);
#endif
      // Make a copy of our color map 
      std::set<ColorPoint> current_colors;
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        for (std::map<ColorPoint,IndexSpaceNode*>::const_iterator it = 
              color_map.begin(); it != color_map.end(); it++)
          current_colors.insert(it->first);
      }
      // Now do the pairwise disjointness tests
      disjoint = true;
      for (std::set<ColorPoint>::const_iterator it1 = current_colors.begin();
            disjoint && (it1 != current_colors.end()); it1++)
      {
        for (std::set<ColorPoint>::const_iterator it2 = it1;
              disjoint && (it2 != current_colors.end()); it2++)
        {
          if ((*it1) == (*it2))
            continue;
          if (!are_disjoint(*it1, *it2, true/*force compute*/))
            disjoint = false;
        }
      }
      // Once we get here, we know the disjointness result so we can
      // trigger the event saying when the disjointness value is ready
      Runtime::trigger_event(ready_event);
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::is_disjoint(bool app_query)
    //--------------------------------------------------------------------------
    {
      if (!disjoint_ready.has_triggered())
        disjoint_ready.wait();
      return disjoint;
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::are_disjoint(const ColorPoint &c1, const ColorPoint &c2,
                                     bool force_compute)
    //--------------------------------------------------------------------------
    {
      if (c1 == c2)
        return false;
      if (!force_compute && is_disjoint(false/*appy query*/))
        return true;
      bool issue_dynamic_test = false;
      std::pair<ColorPoint,ColorPoint> key(c1,c2);
      RtEvent ready_event;
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        if (disjoint_subspaces.find(key) != disjoint_subspaces.end())
          return true;
        else if (aliased_subspaces.find(key) != aliased_subspaces.end())
          return false;
        else
        {
          std::map<std::pair<ColorPoint,ColorPoint>,RtEvent>::const_iterator
            finder = pending_tests.find(key);
          if (finder != pending_tests.end())
            ready_event = finder->second;
          else
          {
            if (Runtime::dynamic_independence_tests)
              issue_dynamic_test = true;
            else
            {
              aliased_subspaces.insert(key);
              aliased_subspaces.insert(std::pair<ColorPoint,ColorPoint>(c2,c1));
              return false;
            }
          }
        }
      }
      if (issue_dynamic_test)
      {
        IndexSpaceNode *left = get_child(c1);
        IndexSpaceNode *right = get_child(c2);
        ApEvent left_pre = left->get_domain_precondition();
        ApEvent right_pre = right->get_domain_precondition();
        AutoLock n_lock(node_lock);
        // Test again to see if we lost the race
        std::map<std::pair<ColorPoint,ColorPoint>,RtEvent>::const_iterator
          finder = pending_tests.find(key);
        if (finder == pending_tests.end())
        {
          DynamicIndependenceArgs args;
          args.hlr_id = HLR_SPACE_INDEPENDENCE_TASK_ID;
          args.parent = this;
          args.left = left;
          args.right = right;
          ApEvent pre = Runtime::merge_events(left_pre, right_pre);
          ready_event = context->runtime->issue_runtime_meta_task(&args, 
                    sizeof(args), HLR_SPACE_INDEPENDENCE_TASK_ID, 
                    HLR_LATENCY_PRIORITY, NULL, Runtime::protect_event(pre));
          pending_tests[key] = ready_event;
          pending_tests[std::pair<ColorPoint,ColorPoint>(c2,c1)] = ready_event;
        }
        else
          ready_event = finder->second;
      }
      ready_event.wait();
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      if (disjoint_subspaces.find(key) != disjoint_subspaces.end())
        return true;
      else
        return false;
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::record_disjointness(bool result,
                                     const ColorPoint &c1, const ColorPoint &c2)
    //--------------------------------------------------------------------------
    {
      if (c1 == c2)
        return;
      AutoLock n_lock(node_lock);
#ifdef DEBUG_LEGION
      assert(color_map.find(c1) != color_map.end());
      assert(color_map.find(c2) != color_map.end());
#endif
      if (result)
      {
        disjoint_subspaces.insert(std::pair<ColorPoint,ColorPoint>(c1,c2));
        disjoint_subspaces.insert(std::pair<ColorPoint,ColorPoint>(c2,c1));
      }
      else
      {
        aliased_subspaces.insert(std::pair<ColorPoint,ColorPoint>(c1,c2));
        aliased_subspaces.insert(std::pair<ColorPoint,ColorPoint>(c2,c1));
      }
      pending_tests.erase(std::pair<ColorPoint,ColorPoint>(c1,c2));
      pending_tests.erase(std::pair<ColorPoint,ColorPoint>(c2,c1));
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::is_complete(void)
    //--------------------------------------------------------------------------
    {
      // If we've cached the value then we are good to go
      {
        AutoLock n_lock(node_lock, 1, false/*exclusive*/);
        if (has_complete)
          return complete;
      }
      // Otherwise compute it 
      std::set<Domain> parent_domains, child_domains;
      bool can_cache = false;
      if (parent->has_component_domains())
        parent_domains = parent->get_component_domains_blocking();
      else
      {
        const Domain &dom = parent->get_domain_blocking();
        parent_domains.insert(dom);
        // We can cache the result if we know the domains
        // has dimension greater than zero indicating we have
        // a structured index space
        can_cache = (dom.get_dim() > 0);
      }
      for (std::map<ColorPoint,IndexSpaceNode*>::const_iterator it = 
            color_map.begin(); it != color_map.end(); it++)
      {
        if (it->second->has_component_domains())
        {
          const std::set<Domain> &child_doms = 
                            it->second->get_component_domains_blocking();
          child_domains.insert(child_doms.begin(), child_doms.end());
        }
        else
          child_domains.insert(it->second->get_domain_blocking());
      }
      bool result = false;
      if (is_disjoint())
      {
	// if the partition is disjoint, we can determine completeness by
	//  seeing if the total volume of the child domains matches the volume
	//  of the parent domains
	size_t parent_volume = 0;
	for (std::set<Domain>::const_iterator it = parent_domains.begin();
	     it != parent_domains.end();
	     ++it)
	  parent_volume += it->get_volume();
	size_t child_volume = 0;
	for (std::set<Domain>::const_iterator it = child_domains.begin();
	     it != child_domains.end();
	     ++it)
	  child_volume += it->get_volume();
	result = (child_volume == parent_volume);
      } 
      else 
      {
	// if not disjoint, we have to do a considerably-more-expensive test
	//  that handles overlap (i.e. double-counting) in the child domains
	result = compute_dominates(child_domains, parent_domains);
      }
      if (can_cache)
      {
        AutoLock n_lock(node_lock);
        complete = result;
        has_complete = true;
      }
      return result;
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::get_colors(std::set<ColorPoint> &colors)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      for (std::map<ColorPoint,IndexSpaceNode*>::const_iterator it = 
            valid_map.begin(); it != valid_map.end(); it++)
      {
        colors.insert(it->first);
      }
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::add_instance(PartitionNode *inst)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
#ifdef DEBUG_LEGION
      assert(logical_nodes.find(inst) == logical_nodes.end());
#endif
      logical_nodes.insert(inst);
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::has_instance(RegionTreeID tid)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      for (std::set<PartitionNode*>::const_iterator it = logical_nodes.begin();
            it != logical_nodes.end(); it++)
      {
        if ((*it)->handle.get_tree_id() == tid)
          return true;
      }
      return false;
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::add_creation_source(AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      creation_set.add(source);
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::DestructionFunctor::apply(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      runtime->send_index_partition_destruction(handle, target);
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::destroy_node(AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      // If we've already been destroyed then we are done
      if (destroyed)
        return;
      AutoLock n_lock(node_lock);
      if (!destroyed)
      {
        destroyed = true;
        if (!creation_set.empty())
        {
          DestructionFunctor functor(handle, context->runtime);
          creation_set.map(functor);
        }
        for (std::set<PartitionNode*>::const_iterator it = 
              logical_nodes.begin(); it != logical_nodes.end(); it++)
        {
          (*it)->destroy_node(source);
        }
      }
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::add_pending_child(const ColorPoint &child_color,
                                          ApUserEvent handle_ready, 
                                          ApUserEvent domain_ready)
    //--------------------------------------------------------------------------
    {
      bool launch_remove = false;
      {
        AutoLock n_lock(node_lock);
        // Duplicate insertions can happen legally so avoid them
        if (pending_children.find(child_color) == pending_children.end())
        {
          pending_children[child_color] = 
            std::pair<ApUserEvent,ApUserEvent>(handle_ready, domain_ready);
          launch_remove = true;
        }
      }
      if (launch_remove)
      {
        PendingChildArgs args;
        args.hlr_id = HLR_PENDING_CHILD_TASK_ID;
        args.parent = this;
        args.pending_child = child_color;
        // Don't remove the pending child until the handle is ready
        context->runtime->issue_runtime_meta_task(&args, sizeof(args),
                                                  HLR_PENDING_CHILD_TASK_ID,
                                                  HLR_LATENCY_PRIORITY, NULL,
                                        Runtime::protect_event(handle_ready));
      }
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::get_pending_child(const ColorPoint &child_color,
                                          ApUserEvent &handle_ready,
                                          ApUserEvent &domain_ready)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock, 1, false/*exclusive*/);
      std::map<ColorPoint,std::pair<ApUserEvent,ApUserEvent> >::const_iterator
        finder = pending_children.find(child_color);
      if (finder != pending_children.end())
      {
        handle_ready = finder->second.first;
        domain_ready = finder->second.second;
        return true;
      }
      return false;
    }
    
    //--------------------------------------------------------------------------
    void IndexPartNode::remove_pending_child(const ColorPoint &child_color)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      pending_children.erase(child_color);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexPartNode::handle_pending_child_task(const void *args)
    //--------------------------------------------------------------------------
    {
      const PendingChildArgs *pargs = (const PendingChildArgs*)args;  
      pargs->parent->remove_pending_child(pargs->pending_child);
    }

    //--------------------------------------------------------------------------
    ApEvent IndexPartNode::create_equal_children(size_t granularity)
    //--------------------------------------------------------------------------
    {
      if (parent->kind == UNSTRUCTURED_KIND)
      {
        size_t num_subspaces = color_space.get_volume();
        std::vector<Realm::IndexSpace> subspaces(num_subspaces);
        ApEvent precondition;
        const Domain &parent_dom = parent->get_domain(precondition);
        // Launch the operation down to the low-level runtime
        ApEvent ready_event(
          parent_dom.get_index_space().create_equal_subspaces(num_subspaces,
                                                            granularity,
                                                            subspaces,
                                                            (mode & ALLOCABLE),
                                                            precondition));
        // Fill in all the subspaces
        unsigned idx = 0;
        for (Domain::DomainPointIterator itr(color_space); itr; itr++, idx++)
        {
          ColorPoint is_color(itr.p);
          IndexSpaceNode *child_node = get_child(is_color);
#ifdef DEBUG_LEGION
          assert(subspaces[idx].exists());
#endif
          child_node->set_domain(Domain(subspaces[idx]));
        }
        return ready_event;
      }
      else
      {
        // TODO: Implement structured kinds
        assert(false);
        return ApEvent::NO_AP_EVENT;
      }
    }

    //--------------------------------------------------------------------------
    ApEvent IndexPartNode::create_weighted_children(
                   const std::map<DomainPoint,int> &weights, size_t granularity)
    //--------------------------------------------------------------------------
    {
      if (parent->kind == UNSTRUCTURED_KIND)
      {
        size_t num_subspaces = weights.size();
        std::vector<int> local_weights(num_subspaces);
        unsigned idx = 0;
        for (std::map<DomainPoint,int>::const_iterator it = weights.begin();
              it != weights.end(); it++, idx++)
        {
          local_weights[idx] = it->second;
        }
        std::vector<Realm::IndexSpace> subspaces(num_subspaces);
        ApEvent precondition;
        const Domain &parent_dom = parent->get_domain(precondition);
        // Launch the operation down to the low-level runtime
        ApEvent ready_event(
          parent_dom.get_index_space().create_weighted_subspaces(num_subspaces,
                                                             granularity,
                                                             local_weights,
                                                             subspaces,
                                                             (mode & ALLOCABLE),
                                                             precondition));
        // Now create each of the sub-spaces
        idx = 0; 
        for (std::map<DomainPoint,int>::const_iterator it = weights.begin();
              it != weights.end(); it++, idx++)
        {
          ColorPoint is_color(it->first);
          IndexSpaceNode *child_node = get_child(is_color);
#ifdef DEBUG_LEGION
          assert(subspaces[idx].exists());
#endif
          child_node->set_domain(Domain(subspaces[idx]));
        }
        return ready_event;
      }
      else
      {
        // TODO: Implement structured kinds
        assert(false);
        return ApEvent::NO_AP_EVENT;
      }
    }

    //--------------------------------------------------------------------------
    ApEvent IndexPartNode::create_by_operation(IndexPartNode *left, 
                                               IndexPartNode *right,
                                      Realm::IndexSpace::IndexSpaceOperation op)
    //--------------------------------------------------------------------------
    {
      if (parent->kind == UNSTRUCTURED_KIND)
      {
        size_t num_subspaces = color_space.get_volume();  
        std::vector<Realm::IndexSpace::BinaryOpDescriptor> 
                                                    operations(num_subspaces);
        std::set<ApEvent> preconditions;
        ApEvent parent_pre;
        const Domain parent_dom = parent->get_domain(parent_pre); 
        if (parent_pre.exists())
          preconditions.insert(parent_pre);
        unsigned idx = 0;
        for (Domain::DomainPointIterator itr(color_space); itr; itr++, idx++)
        {
          ColorPoint child_color(itr.p);
          IndexSpaceNode *left_child = left->get_child(child_color);
          IndexSpaceNode *right_child = right->get_child(child_color);
          ApEvent left_pre, right_pre;
          const Domain &left_dom = left_child->get_domain(left_pre);
          if (left_pre.exists())
            preconditions.insert(left_pre);
          const Domain &right_dom = right_child->get_domain(right_pre);
          if (right_pre.exists())
            preconditions.insert(right_pre);
          operations[idx].op = op;
          operations[idx].parent = parent_dom.get_index_space();
          operations[idx].left_operand = left_dom.get_index_space();
          operations[idx].right_operand = right_dom.get_index_space();
        }
        // Merge all the preconditions and issue to the low-level runtime
        ApEvent precondition = Runtime::merge_events(preconditions);
        ApEvent result(Realm::IndexSpace::compute_index_spaces(operations,
                                                            (mode & ALLOCABLE),
                                                            precondition));
#ifdef LEGION_SPY
        LegionSpy::log_event_dependence(precondition, result);
#endif
        // Now set the domains for all the nodes
        idx = 0;
        for (Domain::DomainPointIterator itr(color_space); itr; itr++, idx++)
        {
          ColorPoint is_color(itr.p);
#ifdef DEBUG_LEGION
          assert(operations[idx].result.exists());
#endif
          IndexSpaceNode *child_node = get_child(is_color);
          child_node->set_domain(Domain(operations[idx].result));
        }
        return result;
      }
      else
      {
        // TODO: implement structured kinds
        assert(false);
        return ApEvent::NO_AP_EVENT;
      }
    }

    //--------------------------------------------------------------------------
    ApEvent IndexPartNode::create_by_operation(IndexSpaceNode *left,
                                               IndexPartNode *right,
                                      Realm::IndexSpace::IndexSpaceOperation op)
    //--------------------------------------------------------------------------
    {
      if (parent->kind == UNSTRUCTURED_KIND)
      {
        size_t num_subspaces = color_space.get_volume();  
        std::vector<Realm::IndexSpace::BinaryOpDescriptor> 
                                                    operations(num_subspaces);
        std::set<ApEvent> preconditions;
        ApEvent parent_pre;
        const Domain parent_dom = parent->get_domain(parent_pre); 
        if (parent_pre.exists())
          preconditions.insert(parent_pre);
        ApEvent left_pre;
        const Domain left_dom = left->get_domain(left_pre);
        if (left_pre.exists())
          preconditions.insert(left_pre);
        unsigned idx = 0;
        for (Domain::DomainPointIterator itr(color_space); itr; itr++, idx++)
        {
          ColorPoint child_color(itr.p);
          IndexSpaceNode *child = right->get_child(child_color);
          ApEvent child_pre;
          const Domain child_dom = child->get_domain(child_pre);
          if (child_pre.exists())
            preconditions.insert(child_pre);
          operations[idx].op = op;
          operations[idx].parent = parent_dom.get_index_space();
          operations[idx].left_operand = left_dom.get_index_space();
          operations[idx].right_operand = child_dom.get_index_space();
        }
        // Merge all the preconditions and issue to the low-level runimte
        ApEvent precondition = Runtime::merge_events(preconditions);
        ApEvent result(Realm::IndexSpace::compute_index_spaces(operations,
                                                            (mode & ALLOCABLE),
                                                            precondition));
#ifdef LEGION_SPY
        LegionSpy::log_event_dependence(precondition, result);
#endif
        // Now set the domains for the nodes
        idx = 0;
        for (Domain::DomainPointIterator itr(color_space); itr; itr++, idx++)
        {
          ColorPoint is_color(itr.p);
#ifdef DEBUG_LEGION
          assert(operations[idx].result.exists());
#endif
          IndexSpaceNode *child = get_child(is_color);
          child->set_domain(Domain(operations[idx].result));
        }
        return result;
      }
      else
      {
        // TODO: implement structured kinds
        assert(false);
        return ApEvent::NO_AP_EVENT;
      }
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::get_subspace_domain_preconditions(
                                               std::set<ApEvent> &preconditions)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock, 1, false/*exclusive*/);
      for (std::map<ColorPoint,IndexSpaceNode*>::const_iterator it = 
            color_map.begin(); it != color_map.end(); it++)
      {
        preconditions.insert(it->second->get_domain_precondition());
      }
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::get_subspace_domains(std::set<Domain> &subspaces)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock, 1, false/*exclusive*/);
      for (std::map<ColorPoint,IndexSpaceNode*>::const_iterator it = 
            color_map.begin(); it != color_map.end(); it++)
      {
        if (it->second->has_component_domains())
        {
          const std::set<Domain> &components = 
                                it->second->get_component_domains_blocking();
          subspaces.insert(components.begin(), components.end());
        }
        else
          subspaces.insert(it->second->get_domain_blocking());
      }
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::intersects_with(IndexSpaceNode *other, bool compute)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder != intersections.end()) &&
            (!compute || finder->second.intersections_valid))
          return finder->second.has_intersects;
      }
      std::set<Domain> local_domains, intersect;
      bool result;
      get_subspace_domains(local_domains);
      if (other->has_component_domains())
      {
        result = compute_intersections(local_domains, 
                   other->get_component_domains_blocking(), intersect, compute);
      }
      else
      {
        result = compute_intersections(local_domains, 
                                       other->get_domain_blocking(), 
                                       intersect, compute);
      }
      AutoLock n_lock(node_lock);
      if (result)
      {
        // Check to make sure we didn't lose the race
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder == intersections.end()) ||
            (compute && !finder->second.intersections_valid))
        {
          if (compute)
            intersections[other] = IntersectInfo(intersect);
          else
            intersections[other] = IntersectInfo(true/*result*/);
        }
      }
      else
        intersections[other] = IntersectInfo(false/*result*/);
      return result;
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::intersects_with(IndexPartNode *other, bool compute)
    //--------------------------------------------------------------------------
    {
      if (other == this)
        return true;
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        // Only return the value if we know we are valid and we didn't
        // want to compute anything or we already did compute it
        if ((finder != intersections.end()) &&
            (!compute || finder->second.intersections_valid))
          return finder->second.has_intersects;
      }
      std::set<Domain> local_domains, other_domains, intersect;
      get_subspace_domains(local_domains);
      other->get_subspace_domains(other_domains);
      bool result = compute_intersections(local_domains, other_domains, 
                                          intersect, compute);
      AutoLock n_lock(node_lock);
      if (result)
      {
        // Check to make sure we didn't lose the race
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder == intersections.end()) ||
            (compute && !finder->second.intersections_valid))
        {
          if (compute)
            intersections[other] = IntersectInfo(intersect);
          else
            intersections[other] = IntersectInfo(false/*result*/);
        }
      }
      else
        intersections[other] = IntersectInfo(false/*result*/);
      return result;
    }

    //--------------------------------------------------------------------------
    const std::set<Domain>& IndexPartNode::get_intersection_domains(
                                                          IndexSpaceNode *other)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder != intersections.end()) &&
            finder->second.intersections_valid)
          return finder->second.intersections;
      }
      std::set<Domain> local_domains, intersect;
      bool result;
      get_subspace_domains(local_domains);
      if (other->has_component_domains())
      {
        result = compute_intersections(local_domains, 
           other->get_component_domains_blocking(), intersect, true/*compute*/);
      }
      else
      {
        result = compute_intersections(local_domains, 
                                       other->get_domain_blocking(), 
                                       intersect, true/*compute*/);
      }
      AutoLock n_lock(node_lock);
      if (result)
      {
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder == intersections.end()) ||
            !finder->second.intersections_valid)
          intersections[other] = IntersectInfo(intersect);
      }
      else
        intersections[other] = IntersectInfo(false/*false*/);
      return intersections[other].intersections;
    }

    //--------------------------------------------------------------------------
    const std::set<Domain>& IndexPartNode::get_intersection_domains(
                                                           IndexPartNode *other)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder != intersections.end()) &&
            finder->second.intersections_valid)
          return finder->second.intersections;
      }
      std::set<Domain> local_domains, other_domains, intersect;
      get_subspace_domains(local_domains);
      other->get_subspace_domains(other_domains);
      bool result = compute_intersections(local_domains, other_domains, 
                                          intersect, true/*compute*/);
      AutoLock n_lock(node_lock);
      if (result)
      {
        std::map<IndexTreeNode*,IntersectInfo>::const_iterator finder = 
          intersections.find(other);
        if ((finder == intersections.end()) ||
            !finder->second.intersections_valid)
          intersections[other] = IntersectInfo(intersect);
      }
      else
        intersections[other] = IntersectInfo(false/*result*/);
      return intersections[other].intersections;
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::dominates(IndexSpaceNode *other)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,bool>::const_iterator finder = 
          dominators.find(other);
        if (finder != dominators.end())
          return finder->second;
      }
      std::set<Domain> local;
      get_subspace_domains(local);
      bool result;
      if (other->has_component_domains())
        result = compute_dominates(local, 
                  other->get_component_domains_blocking()); 
      else
      {
        std::set<Domain> other_doms;
        other_doms.insert(other->get_domain_blocking());
        result = compute_dominates(local, other_doms);
      }
      AutoLock n_lock(node_lock);
      dominators[other] = result;
      return result;
    }

    //--------------------------------------------------------------------------
    bool IndexPartNode::dominates(IndexPartNode *other)
    //--------------------------------------------------------------------------
    {
      if (other == this)
        return true;
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<IndexTreeNode*,bool>::const_iterator finder = 
          dominators.find(other);
        if (finder != dominators.end())
          return finder->second;
      }
      std::set<Domain> local, other_doms;
      get_subspace_domains(local);
      other->get_subspace_domains(other_doms);
      bool result = compute_dominates(local, other_doms);
      AutoLock n_lock(node_lock);
      dominators[other] = result;
      return result;
    }

    //--------------------------------------------------------------------------
    /*static*/void IndexPartNode::handle_disjointness_test(
             IndexPartNode *parent, IndexSpaceNode *left, IndexSpaceNode *right)
    //--------------------------------------------------------------------------
    {
      bool disjoint = RegionTreeForest::are_disjoint(left, right);
      parent->record_disjointness(disjoint, left->color, right->color);
    }

    //--------------------------------------------------------------------------
    void IndexPartNode::send_node(AddressSpaceID target, bool up, bool down)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(parent != NULL);
#endif
      if (up)
        parent->send_node(target, true/*up*/, false/*down*/);
      std::map<ColorPoint,IndexSpaceNode*> valid_copy;
      {
        // Make sure we know if this is disjoint or not yet
        bool disjoint_result = is_disjoint();
        AutoLock n_lock(node_lock);
        if (!creation_set.contains(target))
        {
          Serializer rez;
          {
            RezCheck z(rez);
            rez.serialize(handle);
            rez.serialize(color_space);
            rez.serialize(mode);
            rez.serialize(parent->handle); 
            rez.serialize(color);
            rez.serialize<bool>(disjoint_result);
            rez.serialize<size_t>(semantic_info.size());
            for (LegionMap<SemanticTag,SemanticInfo>::aligned::iterator it = 
                  semantic_info.begin(); it != semantic_info.end(); it++)
            {
              rez.serialize(it->first);
              rez.serialize(it->second.size);
              rez.serialize(it->second.buffer, it->second.size);
              rez.serialize(it->second.is_mutable);
            }
            rez.serialize<size_t>(pending_children.size());
            for (std::map<ColorPoint,std::pair<ApUserEvent,ApUserEvent> >
                  ::const_iterator it = pending_children.begin();
                  it != pending_children.end(); it++)
            {
              rez.serialize(it->first);
              rez.serialize(it->second.first);
              rez.serialize(it->second.second);
            }
          }
          context->runtime->send_index_partition_node(target, rez);
          creation_set.add(target);
        }
        // See if we need to go down
        if (down && child_creation.contains(target))
          down = false;
        if (destroyed)
          // Send the deletion notification
          context->runtime->send_index_partition_destruction(handle, target);
        if (down)
          valid_copy = valid_map;
      }
      if (down)
      {
        for (std::map<ColorPoint,IndexSpaceNode*>::const_iterator it = 
              valid_copy.begin(); it != valid_copy.end(); it++)
        {
          it->second->send_node(target, false/*up*/, true/*down*/);
        }
        AutoLock n_lock(node_lock);
        child_creation.add(target);
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexPartNode::handle_node_creation(
        RegionTreeForest *context, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexPartition handle;
      derez.deserialize(handle);
      Domain color_space;
      derez.deserialize(color_space);
      AllocateMode mode;
      derez.deserialize(mode);
      IndexSpace parent;
      derez.deserialize(parent);
      ColorPoint color;
      derez.deserialize(color);
      bool disjoint;
      derez.deserialize(disjoint);
      IndexSpaceNode *parent_node = context->get_node(parent);
#ifdef DEBUG_LEGION
      assert(parent_node != NULL);
#endif
      IndexPartNode *node = context->create_node(handle, parent_node, color,
                                color_space, disjoint, mode);
#ifdef DEBUG_LEGION
      assert(node != NULL);
#endif
      node->add_creation_source(source);
      size_t num_semantic;
      derez.deserialize(num_semantic);
      for (unsigned idx = 0; idx < num_semantic; idx++)
      {
        SemanticTag tag;
        derez.deserialize(tag);
        size_t buffer_size;
        derez.deserialize(buffer_size);
        const void *buffer = derez.get_current_pointer();
        derez.advance_pointer(buffer_size);
        bool is_mutable;
        derez.deserialize(is_mutable);
        node->attach_semantic_information(tag, source,
                                          buffer, buffer_size, is_mutable);
      }
      size_t num_pending;
      derez.deserialize(num_pending);
      for (unsigned idx = 0; idx < num_pending; idx++)
      {
        ColorPoint child_color;
        derez.deserialize(child_color);
        ApUserEvent handle_ready;
        derez.deserialize(handle_ready);
        ApUserEvent domain_ready;
        derez.deserialize(domain_ready);
        node->add_pending_child(child_color, handle_ready, domain_ready);
      }
    } 

    //--------------------------------------------------------------------------
    /*static*/ void IndexPartNode::handle_node_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      IndexPartition handle;
      derez.deserialize(handle);
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      IndexPartNode *target = forest->get_node(handle);
      target->send_node(source, true/*up*/, false/*down*/);
      Serializer rez;
      rez.serialize(to_trigger);
      forest->runtime->send_index_partition_return(source, rez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexPartNode::handle_node_return(Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      Runtime::trigger_event(to_trigger);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexPartNode::handle_node_child_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexPartition handle;
      derez.deserialize(handle);
      ColorPoint child_color;
      derez.deserialize(child_color);
      IndexSpace *target;
      derez.deserialize(target);
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      IndexPartNode *parent = forest->get_node(handle);
      IndexSpaceNode *child = parent->get_child(child_color);
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(child->handle);
        rez.serialize(target);
        rez.serialize(to_trigger);
      }
      forest->runtime->send_index_partition_child_response(source, rez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexPartNode::handle_node_child_response(
                                                            Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexSpace handle;
      derez.deserialize(handle);
      IndexSpace *target;
      derez.deserialize(target);
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      (*target) = handle;
      Runtime::trigger_event(to_trigger);
    }

    //--------------------------------------------------------------------------
    /*static*/ void IndexPartNode::handle_notification(RegionTreeForest *forest,
                                                       Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      IndexPartition pid;
      derez.deserialize(pid);
      IndexSpace parent;
      derez.deserialize(parent);
      ColorPoint part_color;
      derez.deserialize(part_color);
      Domain color_space;
      derez.deserialize(color_space);
      PartitionKind part_kind;
      derez.deserialize(part_kind);
      AllocateMode mode;
      derez.deserialize(mode);
      RtUserEvent done_event;
      derez.deserialize(done_event);
#ifdef DEBUG_LEGION
      assert(part_kind != COMPUTE_KIND);
#endif
      IndexSpaceNode *parent_node = forest->get_node(parent);
      // We know it is safe to make this node because this message only
      // comes before the handle has been given back to the application
      forest->create_node(pid, parent_node, part_color, color_space,
                          (part_kind == DISJOINT_KIND), mode);
      // Now we can trigger the done event
      Runtime::trigger_event(done_event);
    }

    /////////////////////////////////////////////////////////////
    // Field Space Node 
    /////////////////////////////////////////////////////////////
    
    //--------------------------------------------------------------------------
    FieldSpaceNode::FieldSpaceNode(FieldSpace sp, RegionTreeForest *ctx)
      : handle(sp), is_owner((sp.id % ctx->runtime->runtime_stride) ==
          ctx->runtime->address_space), 
        owner(sp.id % ctx->runtime->runtime_stride), 
        context(ctx), destroyed(false)
    //--------------------------------------------------------------------------
    {
      this->node_lock = Reservation::create_reservation();
      if (is_owner)
        this->available_indexes = FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES);
    }

    //--------------------------------------------------------------------------
    FieldSpaceNode::FieldSpaceNode(FieldSpace sp, RegionTreeForest *ctx,
                                   Deserializer &derez)
      : handle(sp), is_owner((sp.id % ctx->runtime->runtime_stride) ==
          ctx->runtime->address_space), 
        owner(sp.id % ctx->runtime->runtime_stride), 
        context(ctx), destroyed(false)
    //--------------------------------------------------------------------------
    {
      this->node_lock = Reservation::create_reservation();
      if (is_owner)
        this->available_indexes = FieldMask(LEGION_FIELD_MASK_FIELD_ALL_ONES);
      size_t num_fields;
      derez.deserialize(num_fields);
      for (unsigned idx = 0; idx < num_fields; idx++)
      {
        FieldID fid;
        derez.deserialize(fid);
        derez.deserialize(fields[fid]);
      }
      size_t num_top;
      derez.deserialize(num_top);
      for (unsigned idx = 0; idx < num_top; idx++)
      {
        LogicalRegion top;
        derez.deserialize(top);
        logical_trees.insert(top);
      }
    }

    //--------------------------------------------------------------------------
    FieldSpaceNode::FieldSpaceNode(const FieldSpaceNode &rhs)
      : handle(FieldSpace::NO_SPACE), is_owner(false), owner(0), context(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    FieldSpaceNode::~FieldSpaceNode(void)
    //--------------------------------------------------------------------------
    {
      node_lock.destroy_reservation();
      node_lock = Reservation::NO_RESERVATION;
      for (std::map<LEGION_FIELD_MASK_FIELD_TYPE,LegionList<LayoutDescription*,
            LAYOUT_DESCRIPTION_ALLOC>::tracked>::iterator it =
            layouts.begin(); it != layouts.end(); it++)
      {
        LegionList<LayoutDescription*,LAYOUT_DESCRIPTION_ALLOC>::tracked
          &descs = it->second;
        for (LegionList<LayoutDescription*,LAYOUT_DESCRIPTION_ALLOC>::
              tracked::iterator it = descs.begin(); it != descs.end(); it++)
        {
          if ((*it)->remove_reference())
            delete (*it);
        }
      }
      layouts.clear();
      for (LegionMap<SemanticTag,SemanticInfo>::aligned::iterator it = 
            semantic_info.begin(); it != semantic_info.end(); it++)
      {
        legion_free(SEMANTIC_INFO_ALLOC, it->second.buffer, it->second.size);
      }
      for (LegionMap<std::pair<FieldID,SemanticTag>,
            SemanticInfo>::aligned::iterator it = semantic_field_info.begin(); 
            it != semantic_field_info.end(); it++)
      {
        legion_free(SEMANTIC_INFO_ALLOC, it->second.buffer, it->second.size);
      }
    }

    //--------------------------------------------------------------------------
    FieldSpaceNode& FieldSpaceNode::operator=(const FieldSpaceNode &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void* FieldSpaceNode::operator new(size_t count)
    //--------------------------------------------------------------------------
    {
      return legion_alloc_aligned<FieldSpaceNode,true/*bytes*/>(count);
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::operator delete(void *ptr)
    //--------------------------------------------------------------------------
    {
      free(ptr);
    }

    //--------------------------------------------------------------------------
    AddressSpaceID FieldSpaceNode::get_owner_space(void) const
    //--------------------------------------------------------------------------
    {
      return get_owner_space(handle, context->runtime);
    }

    //--------------------------------------------------------------------------
    /*static*/ AddressSpaceID FieldSpaceNode::get_owner_space(FieldSpace handle,
                                                              Runtime *rt)
    //--------------------------------------------------------------------------
    {
      return (handle.id % rt->runtime_stride);
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::attach_semantic_information(SemanticTag tag,
                                                     AddressSpaceID source,
                                                     const void *buffer, 
                                                     size_t size, 
                                                     bool is_mutable)
    //--------------------------------------------------------------------------
    {
      void *local = legion_malloc(SEMANTIC_INFO_ALLOC, size);
      memcpy(local, buffer, size);
      bool added = true;
      RtUserEvent to_trigger;
      {
        AutoLock n_lock(node_lock); 
        // See if it already exists
        LegionMap<SemanticTag,SemanticInfo>::aligned::iterator finder = 
          semantic_info.find(tag);
        if (finder != semantic_info.end())
        {
          // First check to see if it is valid
          if (finder->second.is_valid())
          {
            if (!finder->second.is_mutable)
            {
              // Check to make sure that the bits are the same
              if (size != finder->second.size)
              {
                log_run.error("ERROR: Inconsistent Semantic Tag value "
                              "for tag %ld with different sizes of %ld"
                              " and %ld for index tree node", 
                              tag, size, finder->second.size);
#ifdef DEBUG_LEGION
                assert(false);
#endif
                exit(ERROR_INCONSISTENT_SEMANTIC_TAG);       
              }
              // Otherwise do a bitwise comparison
              {
                const char *orig = (const char*)finder->second.buffer;
                const char *next = (const char*)buffer;
                for (unsigned idx = 0; idx < size; idx++)
                {
                  char diff = orig[idx] ^ next[idx];
                  if (diff)
                  {
                    log_run.error("ERROR: Inconsistent Semantic Tag value "
                                  "for tag %ld with different values at"
                                  "byte %d for index tree node, %x != %x", 
                                  tag, idx, orig[idx], next[idx]);
#ifdef DEBUG_LEGION
                    assert(false);
#endif
                    exit(ERROR_INCONSISTENT_SEMANTIC_TAG);
                  }
                }
              }
              added = false;
            }
            else
            {
              // Mutable so we can overwrite 
              legion_free(SEMANTIC_INFO_ALLOC, finder->second.buffer,
                          finder->second.size);
              finder->second.buffer = local;
              finder->second.size = size;
              finder->second.ready_event = RtUserEvent::NO_RT_USER_EVENT;
              finder->second.is_mutable = is_mutable;
            }
          }
          else
          {
            finder->second.buffer = local;
            finder->second.size = size;
            // See if we have an event to trigger
            to_trigger = finder->second.ready_event;
            finder->second.ready_event = RtUserEvent::NO_RT_USER_EVENT;
            finder->second.is_mutable = is_mutable;
          }
        }
        else
          semantic_info[tag] = SemanticInfo(local, size, is_mutable);
      }
      // Trigger the ready event if there is one
      if (to_trigger.exists())
        Runtime::trigger_event(to_trigger);
      if (added)
      {
        AddressSpaceID owner_space = get_owner_space();
        // If we are not the owner and the message 
        // didn't come from the owner, then send it 
        if ((owner_space != context->runtime->address_space) &&
            (source != owner_space))
          send_semantic_info(owner_space, tag, buffer, size, is_mutable);
      }
      else
        legion_free(SEMANTIC_INFO_ALLOC, local, size);
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::attach_semantic_information(FieldID fid,
                                                     SemanticTag tag,
                                                     AddressSpaceID source,
                                                     const void *buffer,
                                                     size_t size, 
                                                     bool is_mutable)
    //--------------------------------------------------------------------------
    {
      void *local = legion_malloc(SEMANTIC_INFO_ALLOC, size);
      memcpy(local, buffer, size);
      bool added = true;
      RtUserEvent to_trigger;
      {
        AutoLock n_lock(node_lock); 
        // See if it already exists
        LegionMap<std::pair<FieldID,SemanticTag>,
            SemanticInfo>::aligned::iterator finder =
          semantic_field_info.find(std::pair<FieldID,SemanticTag>(fid,tag));
        if (finder != semantic_field_info.end())
        {
          // First check to see if it is valid
          if (finder->second.is_valid())
          {
            if (!finder->second.is_mutable)
            {
              // Check to make sure that the bits are the same
              if (size != finder->second.size)
              {
                log_run.error("ERROR: Inconsistent Semantic Tag value "
                              "for tag %ld with different sizes of %ld"
                              " and %ld for index tree node", 
                              tag, size, finder->second.size);
#ifdef DEBUG_LEGION
                assert(false);
#endif
                exit(ERROR_INCONSISTENT_SEMANTIC_TAG);       
              }
              // Otherwise do a bitwise comparison
              {
                const char *orig = (const char*)finder->second.buffer;
                const char *next = (const char*)buffer;
                for (unsigned idx = 0; idx < size; idx++)
                {
                  char diff = orig[idx] ^ next[idx];
                  if (diff)
                  {
                    log_run.error("ERROR: Inconsistent Semantic Tag value "
                                  "for tag %ld with different values at"
                                  "byte %d for index tree node, %x != %x", 
                                  tag, idx, orig[idx], next[idx]);
#ifdef DEBUG_LEGION
                    assert(false);
#endif
                    exit(ERROR_INCONSISTENT_SEMANTIC_TAG);
                  }
                }
              }
              added = false;
            }
            else
            {
              // Mutable so we can overwrite
              legion_free(SEMANTIC_INFO_ALLOC, finder->second.buffer,
                          finder->second.size);
              finder->second.buffer = local;
              finder->second.size = size;
              finder->second.ready_event = RtUserEvent::NO_RT_USER_EVENT;
              finder->second.is_mutable = is_mutable;
            }
          }
          else
          {
            finder->second.buffer = local;
            finder->second.size = size;
            // See if we have an event to trigger
            to_trigger = finder->second.ready_event;
            finder->second.ready_event = RtUserEvent::NO_RT_USER_EVENT;
            finder->second.is_mutable = is_mutable;
          }
        }
        else
        {
          semantic_field_info[std::pair<FieldID,SemanticTag>(fid,tag)] = 
            SemanticInfo(local, size, is_mutable);
        }
      }
      // Trigger the ready event if there is one
      if (to_trigger.exists())
        Runtime::trigger_event(to_trigger);
      if (added)
      {
        AddressSpaceID owner_space = get_owner_space();
        // If we are not the owner and the message 
        // didn't come from the owner, then send it 
        if ((owner_space != context->runtime->address_space) &&
            (source != owner_space))
          send_semantic_field_info(owner_space, fid, tag, 
                                   buffer, size, is_mutable); 
      }
      else
        legion_free(SEMANTIC_INFO_ALLOC, local, size);
    }

    //--------------------------------------------------------------------------
    bool FieldSpaceNode::retrieve_semantic_information(SemanticTag tag,
              const void *&result, size_t &size, bool can_fail, bool wait_until)
    //--------------------------------------------------------------------------
    {
      RtUserEvent wait_on;
      {
        AutoLock n_lock(node_lock);
        LegionMap<SemanticTag,SemanticInfo>::aligned::const_iterator finder = 
          semantic_info.find(tag); 
        if (finder != semantic_info.end())
        {
          // Already have the data so we are done
          if (finder->second.is_valid())
          {
            result = finder->second.buffer;
            size = finder->second.size;
            return true;
          }
          else if (!can_fail && wait_until)
            wait_on = finder->second.ready_event;
          else
            wait_on = Runtime::create_rt_user_event();
        }
        else
        {
          // Otherwise make an event to wait on
          if (!can_fail && wait_until)
          {
            wait_on = Runtime::create_rt_user_event();
            semantic_info[tag] = SemanticInfo(wait_on);
          }
          else
            wait_on = Runtime::create_rt_user_event();
        }
      }
      // If we are not the owner, send a request, otherwise we are
      // the owner and the information will get sent here
      AddressSpaceID owner_space = get_owner_space();
      if (owner_space != context->runtime->address_space)
      {
        Serializer rez;
        {
          rez.serialize(handle);
          rez.serialize(tag);
          rez.serialize(can_fail);
          rez.serialize(wait_until);
          rez.serialize(wait_on);
        }
        context->runtime->send_field_space_semantic_request(owner_space, rez);
      }
      else
      {
        if (can_fail)
          return false;
        log_run.error("ERROR: invalid semantic tag %ld for "
                      "field space %d", tag, handle.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_SEMANTIC_TAG);
      }
      // Now wait
      wait_on.wait();
      // When we wake up, we should be able to find everything
      AutoLock n_lock(node_lock,1,false/*exclusive*/); 
      LegionMap<SemanticTag,SemanticInfo>::aligned::const_iterator finder = 
        semantic_info.find(tag);
      if (finder == semantic_info.end())
      {
        if (can_fail)
          return false;
        log_run.error("ERROR: invalid semantic tag %ld for "
                            "field space %d", tag, handle.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_SEMANTIC_TAG);
      }
      result = finder->second.buffer;
      size = finder->second.size;
      return true;
    }

    //--------------------------------------------------------------------------
    bool FieldSpaceNode::retrieve_semantic_information(FieldID fid,
                             SemanticTag tag, const void *&result, size_t &size,
                             bool can_fail, bool wait_until)
    //--------------------------------------------------------------------------
    {
      RtUserEvent wait_on;
      {
        AutoLock n_lock(node_lock);
        LegionMap<std::pair<FieldID,SemanticTag>,
          SemanticInfo>::aligned::const_iterator finder = 
            semantic_field_info.find(std::pair<FieldID,SemanticTag>(fid,tag));
        if (finder != semantic_field_info.end())
        {
          // Already have the data so we are done
          if (finder->second.is_valid())
          {
            result = finder->second.buffer;
            size = finder->second.size;
            return true;
          }
          else if (!can_fail && wait_until)
            wait_on = finder->second.ready_event;
          else
            wait_on = Runtime::create_rt_user_event();
        }
        else
        {
          // Otherwise make an event to wait on
          if (!can_fail && wait_until)
          {
            wait_on = Runtime::create_rt_user_event();
            semantic_field_info[std::pair<FieldID,SemanticTag>(fid,tag)] = 
              SemanticInfo(wait_on);
          }
          else
            wait_on = Runtime::create_rt_user_event();
        }
      }
      // If we are not the owner, send a request, otherwise we are
      // the owner and the information will get sent here
      AddressSpaceID owner_space = get_owner_space();
      if (owner_space != context->runtime->address_space)
      {
        Serializer rez;
        {
	  RezCheck z(rez);
          rez.serialize(handle);
          rez.serialize(fid);
          rez.serialize(tag);
          rez.serialize(can_fail);
          rez.serialize(wait_until);
          rez.serialize(wait_on);
        }
        context->runtime->send_field_semantic_request(owner_space, rez);
      }
      else
      {
        if (can_fail)
          return false;
        log_run.error("ERROR: invalid semantic tag %ld for field %d "
                      "of field space %d", tag, fid, handle.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_SEMANTIC_TAG);
      }
      // Now wait
      wait_on.wait();
      // When we wake up, we should be able to find everything
      AutoLock n_lock(node_lock,1,false/*exclusive*/); 
      LegionMap<std::pair<FieldID,SemanticTag>,
        SemanticInfo>::aligned::const_iterator finder = 
          semantic_field_info.find(std::pair<FieldID,SemanticTag>(fid,tag));
      if (finder == semantic_field_info.end())
      {
        if (can_fail)
          return false;
        log_run.error("ERROR: invalid semantic tag %ld for field %d "
                            "of field space %d", tag, fid, handle.id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_SEMANTIC_TAG);
      }
      result = finder->second.buffer;
      size = finder->second.size;
      return true;
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::send_semantic_info(AddressSpaceID target, 
              SemanticTag tag, const void *result, size_t size, bool is_mutable)
    //--------------------------------------------------------------------------
    {
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(tag);
        rez.serialize(size);
        rez.serialize(result, size);
        rez.serialize(is_mutable);
      }
      context->runtime->send_field_space_semantic_info(target, rez);
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::send_semantic_field_info(AddressSpaceID target,
                  FieldID fid, SemanticTag tag, const void *result, 
                  size_t size, bool is_mutable)
    //--------------------------------------------------------------------------
    {
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(fid);
        rez.serialize(tag);
        rez.serialize(size);
        rez.serialize(result, size);
        rez.serialize(is_mutable);
      }
      context->runtime->send_field_semantic_info(target, rez);
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::process_semantic_request(SemanticTag tag,
       AddressSpaceID source, bool can_fail, bool wait_until, RtUserEvent ready)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(get_owner_space() == context->runtime->address_space);
#endif
      RtEvent precondition;
      void *result = NULL;
      size_t size = 0;
      bool is_mutable = false;
      {
        AutoLock n_lock(node_lock);
        // See if we already have the data
        LegionMap<SemanticTag,SemanticInfo>::aligned::iterator finder = 
          semantic_info.find(tag);
        if (finder != semantic_info.end())
        {
          if (finder->second.is_valid())
          {
            result = finder->second.buffer;
            size = finder->second.size;
            is_mutable = finder->second.is_mutable;
          }
          else if (!can_fail && wait_until)
            precondition = finder->second.ready_event;
        }
        else if (!can_fail && wait_until)
        {
          // Don't have it yet, make a condition and hope that one comes
          RtUserEvent ready_event = Runtime::create_rt_user_event();
          precondition = ready_event;
          semantic_info[tag] = SemanticInfo(ready_event);
        }
      }
      if (result == NULL)
      {
        if (can_fail || !wait_until)
          Runtime::trigger_event(ready);
        else
        {
          // Defer this until the semantic condition is ready
          SemanticRequestArgs args;
          args.hlr_id = HLR_FIELD_SPACE_SEMANTIC_INFO_REQ_TASK_ID;
          args.proxy_this = this;
          args.tag = tag;
          args.source = source;
          context->runtime->issue_runtime_meta_task(&args, sizeof(args),
                              HLR_FIELD_SPACE_SEMANTIC_INFO_REQ_TASK_ID,
                              HLR_LATENCY_PRIORITY, NULL/*op*/, precondition);
        }
      }
      else
        send_semantic_info(source, tag, result, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::process_semantic_field_request(FieldID fid, 
                               SemanticTag tag, AddressSpaceID source, 
                               bool can_fail, bool wait_until,RtUserEvent ready)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(get_owner_space() == context->runtime->address_space);
#endif
      RtEvent precondition;
      void *result = NULL;
      size_t size = 0;
      bool is_mutable = false;
      {
        AutoLock n_lock(node_lock);
        // See if we already have the data
        std::pair<FieldID,SemanticTag> key(fid,tag);
        LegionMap<std::pair<FieldID,SemanticTag>,SemanticInfo>::aligned::
          iterator finder = semantic_field_info.find(key);
        if (finder != semantic_field_info.end())
        {
          if (finder->second.is_valid())
          {
            result = finder->second.buffer;
            size = finder->second.size;
            is_mutable = finder->second.is_mutable;
          }
          else if (!can_fail && wait_until)
            precondition = finder->second.ready_event;
        }
        else if (!can_fail && wait_until)
        {
          // Don't have it yet, make a condition and hope that one comes
          RtUserEvent ready_event = Runtime::create_rt_user_event();
          precondition = ready_event;
          semantic_field_info[key] = SemanticInfo(ready_event);
        }
      }
      if (result == NULL)
      {
        if (can_fail || !wait_until)
          Runtime::trigger_event(ready);
        else
        {
          // Defer this until the semantic condition is ready
          SemanticFieldRequestArgs args;
          args.hlr_id = HLR_FIELD_SEMANTIC_INFO_REQ_TASK_ID;
          args.proxy_this = this;
          args.fid = fid;
          args.tag = tag;
          args.source = source;
          context->runtime->issue_runtime_meta_task(&args, sizeof(args),
                              HLR_FIELD_SEMANTIC_INFO_REQ_TASK_ID,
                              HLR_LATENCY_PRIORITY, NULL/*op*/, precondition);
        }
      }
      else
        send_semantic_field_info(source, fid, tag, result, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_semantic_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      FieldSpace handle;
      derez.deserialize(handle);
      SemanticTag tag;
      derez.deserialize(tag);
      bool can_fail;
      derez.deserialize(can_fail);
      bool wait_until;
      derez.deserialize(wait_until);
      RtUserEvent ready;
      derez.deserialize(ready);
      FieldSpaceNode *node = forest->get_node(handle);
      node->process_semantic_request(tag, source, can_fail, wait_until, ready);
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_field_semantic_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      FieldSpace handle;
      derez.deserialize(handle);
      FieldID fid;
      derez.deserialize(fid);
      SemanticTag tag;
      derez.deserialize(tag);
      bool can_fail;
      derez.deserialize(can_fail);
      bool wait_until;
      derez.deserialize(wait_until);
      RtUserEvent ready;
      derez.deserialize(ready);
      FieldSpaceNode *node = forest->get_node(handle);
      node->process_semantic_field_request(fid, tag, source, 
                                           can_fail, wait_until, ready);
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_semantic_info(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      FieldSpace handle;
      derez.deserialize(handle);
      SemanticTag tag;
      derez.deserialize(tag);
      size_t size;
      derez.deserialize(size);
      const void *buffer = derez.get_current_pointer();
      derez.advance_pointer(size);
      bool is_mutable;
      derez.deserialize(is_mutable);
      forest->attach_semantic_information(handle, tag, source, 
                                          buffer, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_field_semantic_info(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      FieldSpace handle;
      derez.deserialize(handle);
      FieldID fid;
      derez.deserialize(fid);
      SemanticTag tag;
      derez.deserialize(tag);
      size_t size;
      derez.deserialize(size);
      const void *buffer = derez.get_current_pointer();
      derez.advance_pointer(size);
      bool is_mutable;
      derez.deserialize(is_mutable);
      forest->attach_semantic_information(handle, fid, tag, 
                                          source, buffer, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::FindTargetsFunctor::apply(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      targets.push_back(target);
    }

    //--------------------------------------------------------------------------
    RtEvent FieldSpaceNode::allocate_field(FieldID fid, size_t size, 
                                           CustomSerdezID serdez_id)
    //--------------------------------------------------------------------------
    {
      // If we're not the owner, send the request to the owner 
      if (!is_owner)
      {
        RtUserEvent allocated_event = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(handle);
          rez.serialize(allocated_event);
          rez.serialize(serdez_id);
          rez.serialize<size_t>(1); // only allocating one field
          rez.serialize(fid);
          rez.serialize(size);
        }
        context->runtime->send_field_alloc_request(owner, rez);
        return allocated_event;
      }
      std::deque<AddressSpaceID> targets;
      unsigned index = 0;
      {
        // We're the owner so do the field allocation
        AutoLock n_lock(node_lock);
#ifdef DEBUG_LEGION
        assert(fields.find(fid) == fields.end());
#endif
        // Find an index in which to allocate this field  
        int result = allocate_index();
        if (result < 0)
        {
          log_field.error("Exceeded maximum number of allocated fields for "
                          "field space %x. Change MAX_FIELDS from %d and "
                          "related macros at the top of legion_config.h and "
                          "recompile.", handle.id, MAX_FIELDS);
#ifdef DEBUG_LEGION
          assert(false);
#endif
          exit(ERROR_MAX_FIELD_OVERFLOW);
        }
        index = result;
        fields[fid] = FieldInfo(size, index, serdez_id);
        if (!!creation_set)
        {
          FindTargetsFunctor functor(targets);
          creation_set.map(functor);   
        }
      }
      if (!targets.empty())
      {
        std::set<RtEvent> allocated_events;
        for (std::deque<AddressSpaceID>::const_iterator it = targets.begin();
              it != targets.end(); it++)
        {
          RtUserEvent done_event = Runtime::create_rt_user_event();
          allocated_events.insert(done_event);
          Serializer rez;
          {
            RezCheck z(rez);
            rez.serialize(handle);
            rez.serialize(done_event);
            rez.serialize(serdez_id);
            rez.serialize<size_t>(1);
            rez.serialize(fid);
            rez.serialize(size);
            rez.serialize(index);
          }
          context->runtime->send_field_alloc_notification(*it, rez);
        }
        return Runtime::merge_events(allocated_events);
      }
      return RtEvent::NO_RT_EVENT;
    }

    //--------------------------------------------------------------------------
    RtEvent FieldSpaceNode::allocate_fields(const std::vector<size_t> &sizes,
                                            const std::vector<FieldID> &fids,
                                            CustomSerdezID serdez_id)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(sizes.size() == fids.size());
#endif
      // If we're not the owner, send the request to the owner 
      if (!is_owner)
      {
        RtUserEvent allocated_event = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(handle);
          rez.serialize(allocated_event);
          rez.serialize(serdez_id);
          rez.serialize<size_t>(fids.size());
          for (unsigned idx = 0; idx < fids.size(); idx++)
          {
            rez.serialize(fids[idx]);
            rez.serialize(sizes[idx]);
          }
        }
        context->runtime->send_field_alloc_request(owner, rez);
        return allocated_event;
      }
      std::deque<AddressSpaceID> targets;
      std::vector<unsigned> indexes(fids.size());
      {
        // We're the owner so do the field allocation
        AutoLock n_lock(node_lock);
        for (unsigned idx = 0; idx < fids.size(); idx++)
        {
          FieldID fid = fids[idx];
#ifdef DEBUG_LEGION
          assert(fields.find(fid) == fields.end());
#endif
          // Find an index in which to allocate this field  
          int result = allocate_index();
          if (result < 0)
          {
            log_field.error("Exceeded maximum number of allocated fields for "
                            "field space %x. Change MAX_FIELDS from %d and "
                            "related macros at the top of legion_config.h and "
                            "recompile.", handle.id, MAX_FIELDS);
#ifdef DEBUG_LEGION
            assert(false);
#endif
            exit(ERROR_MAX_FIELD_OVERFLOW);
          }
          unsigned index = result;
          fields[fid] = FieldInfo(sizes[idx], index, serdez_id);
          indexes[idx] = index;
        }
        if (!!creation_set)
        {
          FindTargetsFunctor functor(targets);
          creation_set.map(functor);   
        }
      }
      if (!targets.empty())
      {
        std::set<RtEvent> allocated_events;
        for (std::deque<AddressSpaceID>::const_iterator it = targets.begin();
              it != targets.end(); it++)
        {
          RtUserEvent done_event = Runtime::create_rt_user_event();
          allocated_events.insert(done_event);
          Serializer rez;
          {
            RezCheck z(rez);
            rez.serialize(handle);
            rez.serialize(done_event);
            rez.serialize(serdez_id);
            rez.serialize<size_t>(fids.size());
            for (unsigned idx = 0; idx < fids.size(); idx++)
            {
              rez.serialize(fids[idx]);
              rez.serialize(sizes[idx]);
              rez.serialize(indexes[idx]);
            }
          }
          context->runtime->send_field_alloc_notification(*it, rez);
        }
        return Runtime::merge_events(allocated_events);
      }
      return RtEvent::NO_RT_EVENT;
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::free_field(FieldID fid, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      if (!is_owner && (source != owner))
      {
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(handle);
          rez.serialize<size_t>(1);
          rez.serialize(fid);
        }
        context->runtime->send_field_free(owner, rez);
        return;
      }
      std::deque<AddressSpaceID> targets;
      {
        // We can actually do this with the read-only lock since we're
        // not actually going to change the allocation of the fields
        // data structure
        AutoLock n_lock(node_lock); 
        std::map<FieldID,FieldInfo>::iterator finder = fields.find(fid);
        finder->second.destroyed = true;
        if (is_owner)
          free_index(finder->second.idx);
        if (is_owner && !!creation_set)
        {
          FindTargetsFunctor functor(targets);
          creation_set.map(functor);
        }
      }
      if (!targets.empty())
      {
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(handle);
          rez.serialize<size_t>(1);
          rez.serialize(fid);
        }
        for (std::deque<AddressSpaceID>::const_iterator it = 
              targets.begin(); it != targets.end(); it++)
        {
          context->runtime->send_field_free(*it, rez);
        }
      }
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::free_fields(const std::vector<FieldID> &to_free,
                                     AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      if (!is_owner && (source != owner))
      {
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(handle);
          rez.serialize<size_t>(to_free.size());
          for (unsigned idx = 0; idx < to_free.size(); idx++)
            rez.serialize(to_free[idx]);
        }
        context->runtime->send_field_free(owner, rez);
        return;
      }
      std::deque<AddressSpaceID> targets;
      {
        // We can actually do this with the read-only lock since we're
        // not actually going to change the allocation of the fields
        // data structure
        AutoLock n_lock(node_lock); 
        for (std::vector<FieldID>::const_iterator it = to_free.begin();
              it != to_free.end(); it++)
        {
          std::map<FieldID,FieldInfo>::iterator finder = fields.find(*it);
          finder->second.destroyed = true;  
          if (is_owner)
            free_index(finder->second.idx);
        }
        if (is_owner && !!creation_set)
        {
          FindTargetsFunctor functor(targets);
          creation_set.map(functor);
        }
      }
      if (!targets.empty())
      {
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(handle);
          rez.serialize<size_t>(to_free.size());
          for (unsigned idx = 0; idx < to_free.size(); idx++)
            rez.serialize(to_free[idx]);
        }
        for (std::deque<AddressSpaceID>::const_iterator it = 
              targets.begin(); it != targets.end(); it++)
        {
          context->runtime->send_field_free(*it, rez);
        }
      }
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::process_alloc_notification(Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      CustomSerdezID serdez_id;
      derez.deserialize(serdez_id);
      size_t num_fields;
      derez.deserialize(num_fields);
      AutoLock n_lock(node_lock);
      for (unsigned idx = 0; idx < num_fields; idx++)
      {
        FieldID fid;
        derez.deserialize(fid);
        FieldInfo &info = fields[fid];
        derez.deserialize(info.field_size);
        derez.deserialize(info.idx);
        info.serdez_id = serdez_id;
      }
    }

    //--------------------------------------------------------------------------
    bool FieldSpaceNode::has_field(FieldID fid)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      std::map<FieldID,FieldInfo>::const_iterator finder = fields.find(fid);
      if (finder == fields.end())
        return false;
      // Make sure we haven't destroyed this field
      return (!finder->second.destroyed);
    }

    //--------------------------------------------------------------------------
    size_t FieldSpaceNode::get_field_size(FieldID fid)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      std::map<FieldID,FieldInfo>::const_iterator finder = fields.find(fid);
#ifdef DEBUG_LEGION
      assert(finder != fields.end());
#endif
      return finder->second.field_size;
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::get_all_fields(std::vector<FieldID> &to_set)
    //--------------------------------------------------------------------------
    {
      to_set.clear();
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      to_set.reserve(fields.size());
      for (std::map<FieldID,FieldInfo>::const_iterator it = fields.begin();
            it != fields.end(); it++)
      {
        if (!it->second.destroyed)
          to_set.push_back(it->first);
      }
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::get_all_regions(std::set<LogicalRegion> &regions)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      regions = logical_trees;
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::get_field_set(const FieldMask &mask,
                                       std::set<FieldID> &to_set)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      for (std::map<FieldID,FieldInfo>::const_iterator it = fields.begin();
            it != fields.end(); it++)
      {
        if (it->second.destroyed)
          continue;
        if (mask.is_set(it->second.idx))
          to_set.insert(it->first);
      }
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::get_field_set(const FieldMask &mask,
                                       std::vector<FieldID> &to_set)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      for (std::map<FieldID,FieldInfo>::const_iterator it = fields.begin();
            it != fields.end(); it++)
      {
        if (it->second.destroyed)
          continue;
        if (mask.is_set(it->second.idx))
          to_set.push_back(it->first);
      }
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::get_field_set(const FieldMask &mask, 
                                       const std::set<FieldID> &basis,
                                       std::set<FieldID> &to_set)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      // Only iterate over the basis fields here
      for (std::set<FieldID>::const_iterator it = basis.begin();
            it != basis.end(); it++)
      {
        std::map<FieldID,FieldInfo>::const_iterator finder = fields.find(*it);
#ifdef DEBUG_LEGION
        assert(finder != fields.end());
#endif
        if (finder->second.destroyed)
          continue;
        if (mask.is_set(finder->second.idx))
          to_set.insert(finder->first);
      }
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::add_instance(RegionNode *inst)
    //--------------------------------------------------------------------------
    {
      RtEvent wait_on = add_instance(inst->handle, 
                                     context->runtime->address_space);
      if (wait_on.exists() && !wait_on.has_triggered())
        wait_on.wait();
    }

    //--------------------------------------------------------------------------
    RtEvent FieldSpaceNode::add_instance(LogicalRegion top_handle,
                                         AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      std::deque<AddressSpaceID> targets;
      {
        AutoLock n_lock(node_lock);
        // Check to see if we already have it, if we do then we are done
        if (logical_trees.find(top_handle) != logical_trees.end())
          return RtEvent::NO_RT_EVENT;
        logical_trees.insert(top_handle);
        if (is_owner)
        {
          if (!!creation_set)
          {
            // Send messages to everyone except the source
            FindTargetsFunctor functor(targets);
            creation_set.map(functor);
          }
        }
        else if (source != owner)
          targets.push_back(owner); // send the message to our owner
      }
      if (!targets.empty())
      {
        std::set<RtEvent> ready_events;
        for (std::deque<AddressSpaceID>::const_iterator it = 
              targets.begin(); it != targets.end(); it++)
        {
          if ((*it) == source)
            continue;
          RtUserEvent done = Runtime::create_rt_user_event();
          ready_events.insert(done);
          Serializer rez;
          {
            RezCheck z(rez);
            rez.serialize(handle);
            rez.serialize(top_handle);
            rez.serialize(done);
          }
          context->runtime->send_field_space_top_alloc(*it, rez);
        }
        if (ready_events.empty())
          return RtEvent::NO_RT_EVENT;
        return Runtime::merge_events(ready_events);
      }
      return RtEvent::NO_RT_EVENT;
    }

    //--------------------------------------------------------------------------
    bool FieldSpaceNode::has_instance(RegionTreeID tid)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      for (std::set<LogicalRegion>::const_iterator it = logical_trees.begin();
            it != logical_trees.end(); it++)
      {
        if (it->get_tree_id() == tid)
          return true;
      }
      return false;
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::DestructionFunctor::apply(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      runtime->send_field_space_destruction(handle, target);
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::destroy_node(AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      // If we've already been destroyed then we are done
      if (destroyed)
        return;
      std::vector<LogicalRegion> to_check;
      {
        AutoLock n_lock(node_lock);
        if (!destroyed)
        {
          destroyed = true;
          if (!creation_set.empty())
          {
            DestructionFunctor functor(handle, context->runtime);
            creation_set.map(functor);
          }
          to_check.insert(to_check.end(), 
                          logical_trees.begin(), logical_trees.end());
        }
      }
      for (std::vector<LogicalRegion>::const_iterator it = 
            to_check.begin(); it != to_check.end(); it++)
      {
        if (context->has_node(*it, true/*local only*/))
          context->get_node(*it)->destroy_node(source);
      }
    }

    //--------------------------------------------------------------------------
    FieldMask FieldSpaceNode::get_field_mask(
                              const std::set<FieldID> &privilege_fields) const
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      FieldMask result;
      for (std::set<FieldID>::const_iterator it = privilege_fields.begin();
            it != privilege_fields.end(); it++)
      {
        std::map<FieldID,FieldInfo>::const_iterator finder = fields.find(*it);
#ifdef DEBUG_LEGION
        assert(finder != fields.end());
#endif
        result.set_bit(finder->second.idx);
      }
#ifdef DEBUG_LEGION
      // Have a little bit of code for logging bit masks when requested
      if (Runtime::bit_mask_logging)
      {
        char *bit_string = result.to_string();
        fprintf(stderr,"%s\n",bit_string);
        free(bit_string);
      }
#endif
      return result;
    }

    //--------------------------------------------------------------------------
    unsigned FieldSpaceNode::get_field_index(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      std::map<FieldID,FieldInfo>::const_iterator finder = fields.find(fid);
#ifdef DEBUG_LEGION
      assert(finder != fields.end());
#endif
      return finder->second.idx;
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::get_field_indexes(const std::vector<FieldID> &needed,
                                          std::vector<unsigned> &indexes) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(needed.size() == indexes.size());
#endif
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      for (unsigned idx = 0; idx < needed.size(); idx++)
      {
      std::map<FieldID,FieldInfo>::const_iterator finder = 
        fields.find(needed[idx]);
#ifdef DEBUG_LEGION
        assert(finder != fields.end());
#endif
        indexes[idx] = finder->second.idx;
      }
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::compute_create_offsets(
                                      const std::vector<FieldID> &create_fields,
                           std::vector<std::pair<FieldID,size_t> > &field_sizes,
                                      std::vector<unsigned> &mask_index_map,
                                      std::vector<CustomSerdezID> &serdez,
                                      FieldMask &mask)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(field_sizes.size() == create_fields.size());
      assert(mask_index_map.size() == create_fields.size());
      assert(serdez.size() == create_fields.size());
#endif
      std::map<unsigned/*mask index*/,unsigned/*layout index*/> index_map;
      {
        // Need to hold the lock when accessing field infos
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        for (unsigned idx = 0; idx < create_fields.size(); idx++)
        {
          FieldID fid = create_fields[idx];
          std::map<FieldID,FieldInfo>::const_iterator finder = fields.find(fid);
	  // Catch unknown fields here for now
	  if (finder == fields.end())
	  {
	    log_run.fatal() << "ERROR: unknown field ID " << fid << " requested during instance creation";
	    assert(0);
	  }
          field_sizes[idx] = 
            std::pair<FieldID,size_t>(fid, finder->second.field_size);
          index_map[finder->second.idx] = idx;
          serdez[idx] = finder->second.serdez_id;
          mask.set_bit(finder->second.idx);
        }
      }
      // Now we can linearize the index map without holding the lock
      unsigned idx = 0;
      for (std::map<unsigned,unsigned>::const_iterator it = 
            index_map.begin(); it != index_map.end(); it++, idx++)
        mask_index_map[idx] = it->second;
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_alloc_request(
                                  RegionTreeForest *forest, Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      FieldSpace handle;
      derez.deserialize(handle);
      RtUserEvent done;
      derez.deserialize(done);
      CustomSerdezID serdez_id;
      derez.deserialize(serdez_id);
      size_t num_fields;
      derez.deserialize(num_fields);
      std::vector<FieldID> fids(num_fields);
      std::vector<size_t> sizes(num_fields);
      for (unsigned idx = 0; idx < num_fields; idx++)
      {
        derez.deserialize(fids[idx]);
        derez.deserialize(sizes[idx]);
      }
      FieldSpaceNode *node = forest->get_node(handle);
      RtEvent ready = node->allocate_fields(sizes, fids, serdez_id);
      Runtime::trigger_event(done, ready);
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_alloc_notification(
                                  RegionTreeForest *forest, Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      FieldSpace handle;
      derez.deserialize(handle);
      RtUserEvent done;
      derez.deserialize(done);
      FieldSpaceNode *node = forest->get_node(handle);
      node->process_alloc_notification(derez);
      Runtime::trigger_event(done); // indicate that we have been notified
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_top_alloc(RegionTreeForest *forest,
                                     Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      FieldSpace handle;
      derez.deserialize(handle);
      LogicalRegion top_handle;
      derez.deserialize(top_handle);
      RtUserEvent done;
      derez.deserialize(done);
      FieldSpaceNode *node = forest->get_node(handle);
      RtEvent ready = node->add_instance(top_handle, source);
      Runtime::trigger_event(done, ready);
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_field_free(RegionTreeForest *forest,
                                     Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      FieldSpace handle;
      derez.deserialize(handle);
      size_t num_fields;
      derez.deserialize(num_fields);
      std::vector<FieldID> fields(num_fields);
      for (unsigned idx = 0; idx < num_fields; idx++)
        derez.deserialize(fields[idx]);
      FieldSpaceNode *node = forest->get_node(handle);
      node->free_fields(fields, source);
    }

    //--------------------------------------------------------------------------
    InstanceManager* FieldSpaceNode::create_file_instance(
                                         const std::set<FieldID> &create_fields, 
                                         RegionNode *node, AttachOp *attach_op)
    //--------------------------------------------------------------------------
    {
      std::vector<FieldID> field_set(create_fields.begin(),create_fields.end());
      std::vector<std::pair<FieldID,size_t> > field_sizes(create_fields.size());
      std::vector<unsigned> mask_index_map(create_fields.size());
      std::vector<CustomSerdezID> serdez(create_fields.size());
      FieldMask file_mask;
      compute_create_offsets(field_set, field_sizes, 
                             mask_index_map, serdez, file_mask);
      // Now make the instance, this should always succeed
      std::vector<size_t> only_sizes(field_sizes.size());
      for (unsigned idx = 0; idx < field_sizes.size(); idx++)
        only_sizes[idx] = field_sizes[idx].second;
      LayoutConstraintSet constraints;
      const Domain &dom = node->get_domain_blocking();
      PhysicalInstance inst = 
        attach_op->create_instance(dom, only_sizes, constraints);
      // Pull out the pointer constraint so that we can use it separately
      // and not have it included in the layout constraints
      PointerConstraint pointer_constraint = constraints.pointer_constraint;
      constraints.pointer_constraint = PointerConstraint();
      // Get the layout
      LayoutDescription *layout = 
        find_layout_description(file_mask, constraints);
      if (layout == NULL)
      {
        LayoutConstraints *layout_constraints = 
          context->runtime->register_layout(handle, constraints);
        layout = create_layout_description(file_mask, layout_constraints, 
                                           mask_index_map, serdez, field_sizes);
      }
#ifdef DEBUG_LEGION
      assert(layout != NULL);
      assert(layout->constraints->specialized_constraint.is_file());
#endif
      DistributedID did = context->runtime->get_available_distributed_id(false);
      MemoryManager *memory = 
        context->runtime->find_memory_manager(inst.get_location());
      InstanceManager *result = legion_new<InstanceManager>(context, did, 
                                         context->runtime->address_space,
                                         context->runtime->address_space,
                                         memory, inst, dom, false/*own*/,
                                         node, layout, pointer_constraint,
                                         true/*register now*/, 
                                         ApEvent::NO_AP_EVENT);
#ifdef DEBUG_LEGION
      assert(result != NULL);
#endif
      return result;
    }

    //--------------------------------------------------------------------------
    LayoutDescription* FieldSpaceNode::find_layout_description(
                  const FieldMask &mask, const LayoutConstraintSet &constraints)
    //--------------------------------------------------------------------------
    {
      std::deque<LayoutDescription*> candidates;
      {
        uint64_t hash_key = mask.get_hash_key();
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<LEGION_FIELD_MASK_FIELD_TYPE,LegionList<LayoutDescription*,
          LAYOUT_DESCRIPTION_ALLOC>::tracked>::const_iterator finder = 
                                                      layouts.find(hash_key);
        if (finder == layouts.end())
          return NULL;
        // Get the ones with a matching mask
        for (std::list<LayoutDescription*>::const_iterator it = 
              finder->second.begin(); it != finder->second.end(); it++)
        {
          if ((*it)->allocated_fields == mask)
            candidates.push_back(*it);
        }
      }
      if (candidates.empty())
        return NULL;
      // First go through the existing descriptions and see if we find
      // one that matches the existing layout
      for (std::deque<LayoutDescription*>::const_iterator it = 
            candidates.begin(); it != candidates.end(); it++)
      {
        if ((*it)->match_layout(constraints))
          return (*it);
      }
      return NULL;
    }

    //--------------------------------------------------------------------------
    LayoutDescription* FieldSpaceNode::find_layout_description(
                          const FieldMask &mask, LayoutConstraints *constraints)
    //--------------------------------------------------------------------------
    {
      // This one better work
      uint64_t hash_key = mask.get_hash_key();
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      std::map<LEGION_FIELD_MASK_FIELD_TYPE,LegionList<LayoutDescription*,
        LAYOUT_DESCRIPTION_ALLOC>::tracked>::const_iterator finder = 
                                                    layouts.find(hash_key);
#ifdef DEBUG_LEGION
      assert(finder != layouts.end());
#endif
      for (std::list<LayoutDescription*>::const_iterator it = 
            finder->second.begin(); it != finder->second.end(); it++)
      {
        if ((*it)->constraints != constraints)
          continue;
        if ((*it)->allocated_fields != mask)
          continue;
        return (*it);
      }
      assert(false);
      return NULL;
    }

    //--------------------------------------------------------------------------
    LayoutDescription* FieldSpaceNode::create_layout_description(
                                     const FieldMask &layout_mask,
                                     LayoutConstraints *constraints,
                                   const std::vector<unsigned> &mask_index_map,
                                   const std::vector<CustomSerdezID> &serdez,
                    const std::vector<std::pair<FieldID,size_t> > &field_sizes)
    //--------------------------------------------------------------------------
    {
      // Make the new field description and then register it
      LayoutDescription *result = new LayoutDescription(this, layout_mask, 
                        constraints, mask_index_map, serdez, field_sizes);
      return register_layout_description(result);
    }

    //--------------------------------------------------------------------------
    LayoutDescription* FieldSpaceNode::register_layout_description(
                                                      LayoutDescription *layout)
    //--------------------------------------------------------------------------
    {
      uint64_t hash_key = layout->allocated_fields.get_hash_key();
      AutoLock n_lock(node_lock);
      LegionList<LayoutDescription*,LAYOUT_DESCRIPTION_ALLOC>::tracked
        &descs = layouts[hash_key];
      if (!descs.empty())
      {
        for (LegionList<LayoutDescription*,LAYOUT_DESCRIPTION_ALLOC>::tracked
              ::const_iterator it = descs.begin(); it != descs.end(); it++)
        {
          if (layout->match_layout(*it))
          {
            // Delete the layout we are trying to register
            // and return the matching one
            delete layout;
            return (*it);
          }
        }
      }
      // Otherwise we successfully registered it
      descs.push_back(layout);
      layout->add_reference();
      return layout;
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::send_node(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      // See if this is in our creation set, if not, send it and all the fields
      AutoLock n_lock(node_lock);
      if (!creation_set.contains(target))
      {
        // First send the node info and then send all the fields
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(handle);
          // Pack the field infos
          size_t num_fields = fields.size();
          rez.serialize<size_t>(num_fields);
          for (std::map<FieldID,FieldInfo>::const_iterator it = 
                fields.begin(); it != fields.end(); it++)
          {
            rez.serialize(it->first);
            rez.serialize(it->second);
          }
          // Pack the logical trees
          rez.serialize<size_t>(logical_trees.size());
          for (std::set<LogicalRegion>::const_iterator it = 
                logical_trees.begin(); it != logical_trees.end(); it++)
          {
            rez.serialize(*it);
          }
          rez.serialize<size_t>(semantic_info.size());
          for (LegionMap<SemanticTag,SemanticInfo>::aligned::iterator it = 
                semantic_info.begin(); it != semantic_info.end(); it++)
          {
            rez.serialize(it->first);
            rez.serialize(it->second.size);
            rez.serialize(it->second.buffer, it->second.size);
            rez.serialize(it->second.is_mutable);
          }
          rez.serialize<size_t>(semantic_field_info.size());
          for (LegionMap<std::pair<FieldID,SemanticTag>,
                SemanticInfo>::aligned::iterator
                it = semantic_field_info.begin(); 
                it != semantic_field_info.end(); it++)
          {
            rez.serialize(it->first.first);
            rez.serialize(it->first.second);
            rez.serialize(it->second.size);
            rez.serialize(it->second.buffer, it->second.size);
            rez.serialize(it->second.is_mutable);
          }
        }
        context->runtime->send_field_space_node(target, rez);
        // Finally add it to the creation set
        creation_set.add(target);
      }
      // Send any deletions if necessary
      if (destroyed)
        context->runtime->send_field_space_destruction(handle, target);
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_node_creation(
          RegionTreeForest *context, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      FieldSpace handle;
      derez.deserialize(handle);
      FieldSpaceNode *node = context->create_node(handle, derez);
#ifdef DEBUG_LEGION
      assert(node != NULL);
#endif
      size_t num_semantic;
      derez.deserialize(num_semantic);
      for (unsigned idx = 0; idx < num_semantic; idx++)
      {
        SemanticTag tag;
        derez.deserialize(tag);
        size_t buffer_size;
        derez.deserialize(buffer_size);
        const void *buffer = derez.get_current_pointer();
        derez.advance_pointer(buffer_size);
        bool is_mutable;
        derez.deserialize(is_mutable);
        node->attach_semantic_information(tag, source, 
                                          buffer, buffer_size, is_mutable);
      }
      size_t num_field_semantic;
      derez.deserialize(num_field_semantic);
      for (unsigned idx = 0; idx < num_field_semantic; idx++)
      {
        FieldID fid;
        derez.deserialize(fid);
        SemanticTag tag;
        derez.deserialize(tag);
        size_t buffer_size;
        derez.deserialize(buffer_size);
        const void *buffer = derez.get_current_pointer();
        derez.advance_pointer(buffer_size);
        bool is_mutable;
        derez.deserialize(is_mutable);
        node->attach_semantic_information(fid, tag, source,
                                          buffer, buffer_size, is_mutable);
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_node_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      FieldSpace handle;
      derez.deserialize(handle);
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      FieldSpaceNode *target = forest->get_node(handle);
      target->send_node(source);
      Serializer rez;
      rez.serialize(to_trigger);
      forest->runtime->send_field_space_return(source, rez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void FieldSpaceNode::handle_node_return(Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      Runtime::trigger_event(to_trigger);
    }

    //--------------------------------------------------------------------------
    char* FieldSpaceNode::to_string(const FieldMask &mask) const
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
#ifdef DEBUG_LEGION
      assert(!!mask);
#endif
      char *result = (char*)malloc(MAX_FIELDS*4); 
      bool first = true;
      for (std::map<FieldID,FieldInfo>::const_iterator it = fields.begin();
            it != fields.end(); it++)
      {
        if (mask.is_set(it->second.idx))
        {
          if (first)
          {
            sprintf(result,"%d",it->first);
            first = false;
          }
          else
          {
            char temp[8];
            sprintf(temp,",%d",it->first);
            strcat(result, temp);
          }
        }
      }
#ifdef DEBUG_LEGION
      assert(!first); // we should have written something
#endif
      return result;
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::get_field_ids(const FieldMask &mask,
                                       std::vector<FieldID> &field_ids) const
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
#ifdef DEBUG_LEGION
      assert(!!mask);
#endif
      for (std::map<FieldID,FieldInfo>::const_iterator it = fields.begin();
            it != fields.end(); it++)
      {
        if (mask.is_set(it->second.idx))
        {
          field_ids.push_back(it->first);
        }
      }
#ifdef DEBUG_LEGION
      assert(!field_ids.empty()); // we should have found something
#endif
    }

    //--------------------------------------------------------------------------
    int FieldSpaceNode::allocate_index(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner);
#endif
      int result = available_indexes.find_first_set();
      if (result >= 0)
        available_indexes.unset_bit(result);
      return result;
    }

    //--------------------------------------------------------------------------
    void FieldSpaceNode::free_index(unsigned index)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner);
      assert(!available_indexes.is_set(index));
#endif
      // Assume we are already holding the node lock
      available_indexes.set_bit(index);
      // We also need to invalidate all our layout descriptions
      // that contain this field
      std::vector<LEGION_FIELD_MASK_FIELD_TYPE> to_delete;
      for (std::map<LEGION_FIELD_MASK_FIELD_TYPE,LegionList<LayoutDescription*,
                  LAYOUT_DESCRIPTION_ALLOC>::tracked>::iterator lit = 
            layouts.begin(); lit != layouts.end(); lit++)
      {
        // If the bit is set, remove the layout descriptions
        if (lit->first & (1ULL << index))
        {
          LegionList<LayoutDescription*,LAYOUT_DESCRIPTION_ALLOC>::tracked
            &descs = lit->second;
          bool perform_delete = true;
          for (LegionList<LayoutDescription*,LAYOUT_DESCRIPTION_ALLOC>::
                tracked::iterator it = descs.begin(); 
                it != descs.end(); /*nothing*/)
          {
            if ((*it)->allocated_fields.is_set(index))
            {
              if ((*it)->remove_reference())
                delete (*it);
              it = descs.erase(it);
            }
            else 
            {
              it++;
              perform_delete = false;
            }
          }
          if (perform_delete)
            to_delete.push_back(lit->first);
        }
      }
      for (std::vector<LEGION_FIELD_MASK_FIELD_TYPE>::const_iterator it = 
            to_delete.begin(); it != to_delete.end(); it++)
      {
        layouts.erase(*it);
      }
    }

    /////////////////////////////////////////////////////////////
    // Region Tree Node 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    RegionTreeNode::RegionTreeNode(RegionTreeForest *ctx, 
                                   FieldSpaceNode *column_src)
      : context(ctx), column_source(column_src), destroyed(false)
    //--------------------------------------------------------------------------
    {
      this->node_lock = Reservation::create_reservation(); 
    }

    //--------------------------------------------------------------------------
    RegionTreeNode::~RegionTreeNode(void)
    //--------------------------------------------------------------------------
    {
      node_lock.destroy_reservation();
      node_lock = Reservation::NO_RESERVATION;
      for (LegionMap<SemanticTag,SemanticInfo>::aligned::iterator it = 
            semantic_info.begin(); it != semantic_info.end(); it++)
      {
        legion_free(SEMANTIC_INFO_ALLOC, it->second.buffer, it->second.size);
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ AddressSpaceID RegionTreeNode::get_owner_space(RegionTreeID tid,
                                                              Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      return (tid % runtime->runtime_stride);
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::attach_semantic_information(SemanticTag tag,
                                                     AddressSpaceID source,
                                                     const void *buffer,
                                                     size_t size,
                                                     bool is_mutable)
    //--------------------------------------------------------------------------
    {
      // Make a copy
      void *local = legion_malloc(SEMANTIC_INFO_ALLOC, size);
      memcpy(local, buffer, size);
      bool added = true;
      RtUserEvent to_trigger;
      {
        AutoLock n_lock(node_lock); 
        // See if it already exists
        LegionMap<SemanticTag,SemanticInfo>::aligned::iterator finder = 
          semantic_info.find(tag);
        if (finder != semantic_info.end())
        {
          if (finder->second.is_valid())
          {
            if (!finder->second.is_mutable)
            {
              // Check to make sure that the bits are the same
              if (size != finder->second.size)
              {
                log_run.error("ERROR: Inconsistent Semantic Tag value "
                              "for tag %ld with different sizes of %ld"
                              " and %ld for region tree node", 
                              tag, size, finder->second.size);
#ifdef DEBUG_LEGION
                assert(false);
#endif
                exit(ERROR_INCONSISTENT_SEMANTIC_TAG);       
              }
              // Otherwise do a bitwise comparison
              {
                const char *orig = (const char*)finder->second.buffer;
                const char *next = (const char*)buffer;
                for (unsigned idx = 0; idx < size; idx++)
                {
                  char diff = orig[idx] ^ next[idx];
                  if (diff)
                  {
                    log_run.error("ERROR: Inconsistent Semantic Tag value "
                                  "for tag %ld with different values at"
                                  "byte %d for region tree node, %x != %x", 
                                  tag, idx, orig[idx], next[idx]);
#ifdef DEBUG_LEGION
                    assert(false);
#endif
                    exit(ERROR_INCONSISTENT_SEMANTIC_TAG);
                  }
                }
              }
              added = false;
            }
            else
            {
              // Mutable so we can just overwrite it
              legion_free(SEMANTIC_INFO_ALLOC, finder->second.buffer,
                          finder->second.size);
              finder->second.buffer = local;
              finder->second.size = size;
              finder->second.ready_event = RtUserEvent::NO_RT_USER_EVENT;
              finder->second.is_mutable = is_mutable;
            }
          }
          else
          {
            finder->second.buffer = local;
            finder->second.size = size;
            to_trigger = finder->second.ready_event;
            finder->second.ready_event = RtUserEvent::NO_RT_USER_EVENT;
            finder->second.is_mutable = is_mutable;
          }
        }
        else
          semantic_info[tag] = SemanticInfo(local, size, is_mutable);
      }
      if (to_trigger.exists())
        Runtime::trigger_event(to_trigger);
      if (added)
      {
        AddressSpaceID owner_space = get_owner_space();
        // If we are not the owner and the message 
        // didn't come from the owner, then send it 
        if ((owner_space != context->runtime->address_space) &&
            (source != owner_space))
        {
          send_semantic_info(owner_space, tag, buffer, size, is_mutable); 
        }
      }
      else
        legion_free(SEMANTIC_INFO_ALLOC, local, size);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeNode::retrieve_semantic_information(SemanticTag tag,
                                                       const void *&result,
                                                       size_t &size,
                                                       bool can_fail,
                                                       bool wait_until)
    //--------------------------------------------------------------------------
    {
      RtUserEvent wait_on;
      {
        AutoLock n_lock(node_lock);
        LegionMap<SemanticTag,SemanticInfo>::aligned::const_iterator finder = 
          semantic_info.find(tag); 
        if (finder != semantic_info.end())
        {
          // Already have the data so we are done
          if (finder->second.is_valid())
          {
            result = finder->second.buffer;
            size = finder->second.size;
            return true;
          }
          else if (!can_fail && wait_until)
            wait_on = finder->second.ready_event;
          else
            wait_on = Runtime::create_rt_user_event();
        }
        else
        {
          // Otherwise make an event to wait on
          if (!can_fail && wait_until)
          {
            wait_on = Runtime::create_rt_user_event();
            semantic_info[tag] = SemanticInfo(wait_on);
          }
          else
            wait_on = Runtime::create_rt_user_event();
        }
      }
      // If we are not the owner, send a request, otherwise we are
      // the owner and the information will get sent here
      AddressSpaceID owner_space = get_owner_space();
      if (owner_space != context->runtime->address_space)
        send_semantic_request(owner_space, tag, can_fail, wait_until, wait_on);
      else
      {
        if (can_fail)
          return false;
        log_run.error("ERROR: invalid semantic tag %ld for "
                      "region tree node", tag);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_SEMANTIC_TAG);
      }
      // Now wait
      wait_on.wait();
      // When we wake up, we should be able to find everything
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      LegionMap<SemanticTag,SemanticInfo>::aligned::const_iterator finder = 
        semantic_info.find(tag);
      if (finder == semantic_info.end())
      {
        if (can_fail)
          return false;
        log_run.error("ERROR: invalid semantic tag %ld for "
                            "region tree node", tag);   
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_SEMANTIC_TAG);
      }
      result = finder->second.buffer;
      size = finder->second.size;
      return true;
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::register_logical_user(ContextID ctx, 
                                               const LogicalUser &user,
                                               RegionTreePath &path,
                                               VersionInfo &version_info,
                                               const TraceInfo &trace_info,
                                               const bool projecting,
                                               const bool report_uninitialized)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, 
                        REGION_NODE_REGISTER_LOGICAL_USER_CALL);
      CurrentState &state = get_current_state(ctx);
#ifdef DEBUG_LEGION
      sanity_check_logical_state(state);
#endif
      const unsigned depth = get_depth();
      const bool arrived = !path.has_child(depth);
      FieldMask open_below;
      ColorPoint next_child;
      if (!arrived)
        next_child = path.get_child(depth);
      // If we've arrived and we're doing analysis for a projection 
      // requirement then we skip the closes for now as we will end up
      // doing them later. We can also skip the close operations for 
      // any operations which have arrived and have read-write privileges
      // because they will be doing their own close operations.
      if (!arrived || !projecting)
      {
        // Now check to see if we need to do any close operations
        // Close up any children which we may have dependences on below
        LogicalCloser closer(ctx, user, arrived/*validates*/, true/*capture*/);
        siphon_logical_children(closer, state, user.field_mask,
                                true/*capture*/, next_child, open_below);
        // We always need to create and register close operations
        // regardless of whether we are tracing or not
        // If we're not replaying a trace we need to do work here
        // See if we need to register a close operation
        if (closer.has_close_operations())
        {
          // Generate the close operations         
          // We need to record the version numbers for this node as well
          closer.record_top_version_numbers(this, state);
          closer.initialize_close_operations(this, user.op, 
                                             version_info, trace_info);
          if (!arrived)
            closer.add_next_child(next_child);
          // Perform dependence analysis for all the close operations
          closer.perform_dependence_analysis(user, open_below,
                                             state.curr_epoch_users,
                                             state.prev_epoch_users);
          // Note we don't need to update the version numbers because
          // that happened when we recorded dirty fields below. 
          // However, we do need to mark that there is no longer any
          // dirty data below this node for all the closed fields

          // Update the dirty_below and partial close fields
          // and filter the current and previous epochs
          closer.update_state(state);
          // Now we can add the close operations to the current epoch
          closer.register_close_operations(state.curr_epoch_users);
        }
      }
      FieldMask dominator_mask;
      if (!arrived || !projecting)
      {
        // We also always do our dependence analysis even if we have
        // already traced because we need to pick up dependences on 
        // any dynamic close operations that we need to do
        // Now that we registered any close operation, do our analysis
        dominator_mask = 
               perform_dependence_checks<CURR_LOGICAL_ALLOC,
                         true/*record*/,false/*has skip*/,true/*track dom*/>(
                            user, state.curr_epoch_users, user.field_mask, 
                            open_below, arrived/*validates*/ && !projecting);
        FieldMask non_dominated_mask = user.field_mask - dominator_mask;
        // For the fields that weren't dominated, we have to check
        // those fields against the previous epoch's users
        if (!!non_dominated_mask)
        {
          perform_dependence_checks<PREV_LOGICAL_ALLOC,
                        true/*record*/, false/*has skip*/, false/*track dom*/>(
                          user, state.prev_epoch_users, non_dominated_mask, 
                          open_below, arrived/*validates*/ && !projecting);
        }
      }
      if (arrived)
      { 
        const bool is_write = IS_WRITE(user.usage); // only writes
        // Now that we've arrived, check to see if we are a projection 
        // region requirement or a normal region requirement. If we are normal
        // then we can do the regular analysis, otherwise, we have to traverse
        // the paths for all the projected regions
        if (projecting)
        {
          // Compute the fat tree path for this operation and then do the
          // traversal over the entire sub-tree
          FatTreePath *fat_path = user.op->compute_fat_path(user.idx); 
          register_logical_fat_path(ctx, user, fat_path, version_info, 
                                    trace_info, report_uninitialized);
          delete fat_path;
        }
        else
        {
          // If we dominated and this is our final destination then we 
          // can filter the operations since we actually do dominate them
          if (!!dominator_mask)
          {
            // Dominator mask is not empty
            // Mask off all the dominated fields from the previous set
            // of epoch users and remove any previous epoch users
            // that were totally dominated
            filter_prev_epoch_users(state, dominator_mask); 
            // them from the list of current epoch users.
            filter_curr_epoch_users(state, dominator_mask);
            // We only advance version numbers for fields which are being
            // written and dominated the previous epoch because multiple 
            // writes for atomic and simultaneous go in the same generation.
            if (is_write)
            {
              // Only advance fields that are not already dirty below
              if (!!state.dirty_below)
              {
                FieldMask split_mask = dominator_mask - state.dirty_below;
                if (!!split_mask)
                  state.advance_version_numbers(split_mask);
              }
              else
                state.advance_version_numbers(dominator_mask);
            }
          }
          // Easy case, we are there 
          // If we have arrived and we are doing read-write access, then we
          // need to capture any versions in sub-trees for which we will 
          // be issuing a close when we actually map.
          if (is_write)
          {
            state.record_version_numbers(user.field_mask, user, version_info,
                                  true/*previous*/, false/*path only*/,
                                  true/*final*/, false/*close top*/,
                                  report_uninitialized);
          }
          else
          {
            // We also need to record the needed version numbers for this node
            // Note that we do this after the version numbers have been updated
            // so that we get the version numbers that we are contributing to
            // as part of the execution for this operation. If we are writing
            // in any way, then record the previous version number
            // If this is a projection requirement, we also need to record any
            // version numbers from farther down in the tree as well. 
            // Do this before the version numbers can be updated.
            state.record_version_numbers(user.field_mask, user, version_info,
                                   false/*previous*/, false/*path only*/,
                                   false/*final*/, false/*close top*/, 
                                   report_uninitialized);
          }
          // If this is a reduction, record that we have an outstanding 
          // reduction at this node in the region tree
          if (user.usage.redop > 0)
            record_logical_reduction(state, user.usage.redop, user.field_mask);
        }
        // Here is the only difference with tracing.  If we already
        // traced then we don't need to register ourselves as a user
        if (!trace_info.already_traced)
        {
          // Register ourself with as a current user of this region
          // Record a mapping reference on this operation
          user.op->add_mapping_reference(user.gen);
          // Add ourselves to the current epoch
          state.curr_epoch_users.push_back(user);
        }
      }
      else // We're still not there, so keep going
      {
        // If we are writing check to see if we have already marked those 
        // fields dirty. If not, advance the version numbers for those fields.
        if (HAS_WRITE(user.usage))
        {
          FieldMask new_dirty_fields = user.field_mask - state.dirty_below;
          if (!!new_dirty_fields)
          {
            state.dirty_below |= new_dirty_fields;
            state.advance_version_numbers(new_dirty_fields);
          }
          // Check to see if we've already done a partial close
          FieldMask partial_close = user.field_mask & state.partially_closed;
          if (!partial_close)
          {
            // We already know that all the version numbers have been advanced
            state.record_version_numbers(user.field_mask, user, version_info, 
                                   true/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   report_uninitialized);
          }
          else
          {
            // Partially closed fields record the current version
            state.record_version_numbers(partial_close, user, version_info,
                                   false/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   report_uninitialized);
            FieldMask non_partial = user.field_mask - partial_close;
            if (!!non_partial)
              state.record_version_numbers(non_partial, user, version_info,
                                   true/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   report_uninitialized);
          }
        }
        else // read-only case
        {
          // See if there are any dirty fields for which we need to capture
          // the previous version numbers, these are fields for which we
          // are dirtly below, but have yet to perform a partial close
          FieldMask dirty_overlap = 
            user.field_mask & (state.dirty_below - state.partially_closed);
          if (!dirty_overlap)
          {
            // No dirty fields below, which means we don't have any previous
            state.record_version_numbers(user.field_mask, user, version_info,
                                   false/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   report_uninitialized);
          }
          else
          {
            // We have overlapping dirty fields, capture previous
            // Record split node for readers!
            state.record_version_numbers(dirty_overlap, user, version_info,
                                   true/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   report_uninitialized);
            // See if we have any non-overlapping
            FieldMask non_dirty = user.field_mask - dirty_overlap;
            if (!!non_dirty)
              state.record_version_numbers(non_dirty, user, version_info,
                                     false/*previous*/, true/*path only*/,
                                     false/*final*/, false/*close top*/,
                                     report_uninitialized);
          }
        }
        RegionTreeNode *child = get_tree_child(next_child);
        if (!open_below)
          child->open_logical_node(ctx, user, path, version_info,
                                   trace_info.already_traced, projecting);
        else
          child->register_logical_user(ctx, user, path, version_info, 
                                       trace_info, projecting);
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::open_logical_node(ContextID ctx,
                                             const LogicalUser &user,
                                             RegionTreePath &path,
                                             VersionInfo &version_info,
                                             const bool already_traced,
                                             const bool projecting)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, REGION_NODE_OPEN_LOGICAL_NODE_CALL);
      CurrentState &state = get_current_state(ctx);
#ifdef DEBUG_LEGION
      sanity_check_logical_state(state);
#endif
      const unsigned depth = get_depth(); 
      if (!path.has_child(depth))
      {
        const bool is_write = IS_WRITE(user.usage);
        // If this is a write, then update our version numbers
        if (is_write)
          state.advance_version_numbers(user.field_mask);
        // If this is a projection then we do need to capture any 
        // child version information because it might change
        if (projecting)
        {
          // Compute the fat tree path for this operation and then do the
          // traversal over the entire sub-tree
          FatTreePath *fat_path = user.op->compute_fat_path(user.idx); 
          open_logical_fat_path(ctx, user, fat_path, version_info);
          delete fat_path;
        }
        else
        {
          // First record any version information that we need
          state.record_version_numbers(user.field_mask, user, version_info,
                                 is_write, false/*path only*/, 
                                 is_write, false/*close top*/, 
                                 false/*report uninitialized*/);
          // If this is a reduction, record that we have an outstanding 
          // reduction at this node in the region tree
          if (user.usage.redop > 0)
            record_logical_reduction(state, user.usage.redop, user.field_mask);
        }
        if (!already_traced)
        {
          // We've arrived where we're going,
          // add ourselves as a user
          // Record a mapping reference on this operation
          user.op->add_mapping_reference(user.gen);
          state.curr_epoch_users.push_back(user);
        }
      }
      else
      {
        // If we are writing check to see if we have already 
        // marked those fields dirty. If not,
        // advance the version numbers for those fields.
#ifdef DEBUG_LEGION
        assert(user.field_mask * state.dirty_below);
#endif
        if (HAS_WRITE(user.usage))
        {
          state.dirty_below |= user.field_mask;
          state.advance_version_numbers(user.field_mask);
          // We already know we advanced
          state.record_version_numbers(user.field_mask, user, version_info,
                                 true/*previous*/, true/*path only*/,
                                 false/*final*/, false/*close top*/,
                                 false/*report uninitialized*/);
        }
        else
        {
          // We're opening only so we don't need to record previous
          state.record_version_numbers(user.field_mask, user, version_info,
                                 false/*previous*/, true/*path only*/,
                                 false/*final*/, false/*close top*/,
                                 false/*report uninitialized*/);
        }
        const ColorPoint &next_child = path.get_child(depth);
        // Update our field states
        merge_new_field_state(state, 
                              FieldState(user, user.field_mask, next_child));
#ifdef DEBUG_LEGION
        sanity_check_logical_state(state);
#endif
        // Then continue the traversal
        RegionTreeNode *child_node = get_tree_child(next_child);
        child_node->open_logical_node(ctx, user, path, version_info, 
                                      already_traced, projecting);
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::register_logical_fat_path(ContextID ctx,
                                                   const LogicalUser &user,
                                                   FatTreePath *fat_path,
                                                   VersionInfo &version_info,
                                                   const TraceInfo &trace_info,
                                               const bool report_uninitialized)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, 
                        REGION_NODE_REGISTER_LOGICAL_FAT_PATH_CALL);
      CurrentState &state = get_current_state(ctx);
#ifdef DEBUG_LEGION
      sanity_check_logical_state(state);
#endif
      const std::map<ColorPoint,FatTreePath*> &children = 
                                                fat_path->get_children();
      const bool arrived = children.empty();
      std::map<ColorPoint,bool> open_only;
      FieldMask any_open_below;
      // Perform the close operations for all the children
      {
        LogicalCloser closer(ctx, user, arrived/*validates*/, true/*capture*/);
        if (!arrived)
        {
          // Close up any interfering children, we know our children
          // are disjoint so don't worry about them interferring with
          // each other. This was checked when the fat path was created
          for (std::map<ColorPoint,FatTreePath*>::const_iterator it = 
                children.begin(); it != children.end(); it++)
          {
            FieldMask open_below;
            siphon_logical_children(closer, state, user.field_mask,
                        true/*not arrived so close*/, it->first, open_below);
            open_only[it->first] = !open_below;
            any_open_below |= open_below;
          }
        }
        else
        {
          // Anything other than a write will do the normal close
          // Writes get their own special close routine
          // Otherwise just do the normal single close operation
          siphon_logical_children(closer, state, user.field_mask,
                                 true/*capture*/, ColorPoint(), any_open_below);
        }
        if (closer.has_close_operations())
        {
          // Generate the close operations         
          // We need to record the version numbers for this node as well
          closer.record_top_version_numbers(this, state);
          closer.initialize_close_operations(this, user.op, 
                                             version_info, trace_info);
          if (!arrived)
          {
            for (std::map<ColorPoint,FatTreePath*>::const_iterator it = 
                  children.begin(); it != children.end(); it++)
            {
              closer.add_next_child(it->first);
            }
          }
          // Perform dependence analysis for all the close operations
          closer.perform_dependence_analysis(user, any_open_below,
                                             state.curr_epoch_users,
                                             state.prev_epoch_users);
          // Note we don't need to update the version numbers because
          // that happened when we recorded dirty fields below. 
          // However, we do need to mark that there is no longer any
          // dirty data below this node for all the closed fields
          
          // Update the dirty below and partial closed fields
          // and filter the current and previous epochs
          closer.update_state(state);
          // Now we can add the close operations to the current epoch
          closer.register_close_operations(state.curr_epoch_users);
        }
      }
      // We also always do our dependence analysis even if we have
      // already traced because we need to pick up dependences on 
      // any dynamic close operations that we need to do
      // Now that we registered any close operation, do our analysis
      FieldMask dominator_mask = 
             perform_dependence_checks<CURR_LOGICAL_ALLOC,
                       true/*record*/,false/*has skip*/,true/*track dom*/>(
                          user, state.curr_epoch_users, user.field_mask, 
                          any_open_below, arrived/*validates*/);
      FieldMask non_dominated_mask = user.field_mask - dominator_mask;
      // For the fields that weren't dominated, we have to check
      // those fields against the previous epoch's users
      if (!!non_dominated_mask)
      {
        perform_dependence_checks<PREV_LOGICAL_ALLOC,
                        true/*record*/, false/*has skip*/, false/*track dom*/>(
                            user, state.prev_epoch_users, non_dominated_mask, 
                            any_open_below, arrived/*validates*/);
      }
      // Unlike for a normal traversal, the user we are registering is 
      // actually the upper bound which was registered at this level of
      // the tree or above, so we can always consider ourselves a dominator
      if (!!dominator_mask)
      {
        // Dominator mask is not empty
        // Mask off all the dominated fields from the previous set
        // of epoch users and remove any previous epoch users
        // that were totally dominated
        filter_prev_epoch_users(state, dominator_mask); 
        // Mask off all dominated fields from current epoch users and move
        // them to prev epoch users.  If all fields masked off, then remove
        // them from the list of current epoch users.
        filter_curr_epoch_users(state, dominator_mask);
      }
      if (arrived)
      {
        const bool is_write = IS_WRITE(user.usage);
        // If we dominated and this is our final destination then we 
        // can filter the operations since we actually do dominate them
        // We only advance version numbers for fields which are being
        // written and dominated the previous epoch because multiple 
        // writes for atomic and simultaneous go in the same generation.
        if (!!dominator_mask && is_write)
        {
          // Only advance fields that are not already dirty below
          if (!!state.dirty_below)
          {
            FieldMask split_mask = dominator_mask - state.dirty_below;
            if (!!split_mask)
              state.advance_version_numbers(split_mask);
          }
          else
            state.advance_version_numbers(dominator_mask);
        }
        // If we have arrived and we are doing read-write access, then we
        // need to capture any versions in sub-trees for which we will 
        // be issuing a close when we actually map.
        if (is_write)
        {
          state.record_version_numbers(user.field_mask, user, version_info,
                                  true/*previous*/, false/*path only*/,
                                  true/*final*/, false/*close top*/,
                                  report_uninitialized);
        }
        else
        {
          // No need to register ourselves as a user 
          state.record_version_numbers(user.field_mask, user, version_info,
                                 false/*previous*/, false/*path only*/, 
                                 false/*final*/, false/*close top*/, 
                                 report_uninitialized);
        }
        // If this is a reduction, record that we have an outstanding 
        // reduction at this node in the region tree
        if (user.usage.redop > 0)
          record_logical_reduction(state, user.usage.redop, user.field_mask);
      }
      else
      {
        // If we are writing check to see if we have already marked those 
        // fields dirty. If not, advance the version numbers for those fields.
        if (HAS_WRITE(user.usage))
        {
          FieldMask new_dirty_fields = user.field_mask - state.dirty_below;
          if (!!new_dirty_fields)
          {
            state.dirty_below |= new_dirty_fields;
            state.advance_version_numbers(new_dirty_fields);
          }
          // Check to see if we've already done a partial close
          FieldMask partial_close = user.field_mask & state.partially_closed;
          if (!partial_close)
          {
            // We already know that all the version numbers have been advanced
            state.record_version_numbers(user.field_mask, user, version_info, 
                                   true/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   report_uninitialized);
          }
          else
          {
            // Partially closed fields record the current version
            state.record_version_numbers(partial_close, user, version_info,
                                   false/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   report_uninitialized);
            FieldMask non_partial = user.field_mask - partial_close;
            if (!!non_partial)
              state.record_version_numbers(non_partial, user, version_info,
                                   true/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   report_uninitialized);
          }
        }
        else // read only case
        {
          // See if there are any dirty fields for which we need to 
          // capture the previous version numbers
          FieldMask dirty_overlap = 
            user.field_mask & (state.dirty_below - state.partially_closed);
          if (!dirty_overlap)
          {
            // No dirty fields below, so we don't need to 
            // capture any previous versions
            state.record_version_numbers(user.field_mask, user, version_info,
                                   false/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   report_uninitialized);
          }
          else
          {
            // We have overlapping dirty fields, capture previous
            state.record_version_numbers(dirty_overlap, user, version_info,
                                   true/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   report_uninitialized);
            // See if we have any non-overlapping
            FieldMask non_dirty = user.field_mask - dirty_overlap;
            if (!!non_dirty)
              state.record_version_numbers(non_dirty, user, version_info,
                                     false/*previous*/, true/*path only*/,
                                     false/*final*/, false/*close top*/,
                                     report_uninitialized);
          }
        }
        for (std::map<ColorPoint,FatTreePath*>::const_iterator it = 
              children.begin(); it != children.end(); it++)
        {
          RegionTreeNode *child = get_tree_child(it->first);
          if (open_only[it->first])
            child->open_logical_fat_path(ctx, user, it->second, version_info);
          else
            child->register_logical_fat_path(ctx, user, it->second,
                                             version_info, trace_info);
        }
      }
    }
    
    //--------------------------------------------------------------------------
    void RegionTreeNode::open_logical_fat_path(ContextID ctx,
                                               const LogicalUser &user,
                                               FatTreePath *fat_path,
                                               VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime,
                        REGION_NODE_OPEN_LOGICAL_FAT_PATH_CALL);
      CurrentState &state = get_current_state(ctx); 
#ifdef DEBUG_LEGION
      sanity_check_logical_state(state);
#endif
      const std::map<ColorPoint,FatTreePath*> &children = 
                                                fat_path->get_children();
      const bool arrived = children.empty();
      const bool is_write = IS_WRITE(user.usage);
      if (arrived)
      {
        if (is_write)
          state.advance_version_numbers(user.field_mask);
        // First record any version information that we need
        state.record_version_numbers(user.field_mask, user, version_info, 
                               is_write, false/*path only*/, 
                               is_write, false/*close top*/, 
                               false/*report uninitialized*/);
        // If this is a reduction, record that we have an outstanding 
        // reduction at this node in the region tree
        if (user.usage.redop > 0)
          record_logical_reduction(state, user.usage.redop, user.field_mask);
      }
      else
      {
        // If we are writing check to see if we have already marked those 
        // fields dirty. If not, advance the version numbers for those fields.
#ifdef DEBUG_LEGION
        assert(user.field_mask * state.dirty_below);
#endif
        if (is_write)
        {
          state.dirty_below |= user.field_mask;
          state.advance_version_numbers(user.field_mask);
          // We already know that we advanced
          state.record_version_numbers(user.field_mask, user, version_info,
                                 true/*previous*/, true/*path only*/,
                                 false/*final*/, false/*close top*/,
                                 false/*report uninitialized*/);
        }
        else
        {
          // We're opening only, so we don't need to record previous
          state.record_version_numbers(user.field_mask, user, version_info,
                                 false/*previous*/, true/*path only*/,
                                 false/*final*/, false/*close top*/,
                                 false/*report uninitialized*/);
        }
        for (std::map<ColorPoint,FatTreePath*>::const_iterator it = 
              children.begin(); it != children.end(); it++)
        {
          // Update our field states
          merge_new_field_state(state, 
                                FieldState(user, user.field_mask, it->first));
#ifdef DEBUG_LEGION
          sanity_check_logical_state(state);
#endif

          RegionTreeNode *child = get_tree_child(it->first);
          child->open_logical_fat_path(ctx, user, it->second, version_info);
        }
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::close_reduction_analysis(ContextID ctx, 
                                                  const LogicalUser &user,
                                                  VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
      CurrentState &state = get_current_state(ctx);
      LogicalCloser closer(ctx, user, false/*validates*/, false/*captures*/);
      ColorPoint dummy_next_child;
      FieldMask dummy_open_below;
      siphon_logical_children(closer, state, user.field_mask, false/*record*/,
                              dummy_next_child, dummy_open_below);
      // At this point we have closed up any children and captured dependences
      // Get the version info
      closer.merge_version_info(version_info, user.field_mask);
      // Capture dependences on any users at this level
      perform_closing_checks<CURR_LOGICAL_ALLOC>(closer, 
                                     state.curr_epoch_users, user.field_mask);
      perform_closing_checks<PREV_LOGICAL_ALLOC>(closer, 
                                     state.prev_epoch_users, user.field_mask);
      state.record_version_numbers(user.field_mask, user, version_info,
                             false/*advance*/, 
                             false/*path only*/, true/*final*/, 
                             false/*close top*/, false/*report uninitialized*/);
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::close_logical_node(LogicalCloser &closer,
                                            const FieldMask &closing_mask,
                                            bool permit_leave_open,
                                            bool read_only_close)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, REGION_NODE_CLOSE_LOGICAL_NODE_CALL);
      CurrentState &state = get_current_state(closer.ctx);
      // Perform closing checks on both the current epoch users
      // as well as the previous epoch users
      perform_closing_checks<CURR_LOGICAL_ALLOC>(closer, 
                                     state.curr_epoch_users, closing_mask);
      perform_closing_checks<PREV_LOGICAL_ALLOC>(closer, 
                                     state.prev_epoch_users, closing_mask);
      
      // Recursively traverse any open children and close them as well
      LegionDeque<FieldState>::aligned new_states;
      for (std::list<FieldState>::iterator it = state.field_states.begin();
            it != state.field_states.end(); /*nothing*/)
      {
        FieldMask overlap = it->valid_fields & closing_mask;
        if (!overlap)
        {
          it++;
          continue;
        }
        // Recursively perform any close operations
        FieldMask already_open;
        perform_close_operations(closer, overlap, *it, 
                                 ColorPoint()/*next child*/,
                                 false/*allow next*/, false/*upgrade*/,
                                 permit_leave_open,
                                 read_only_close,
                                 false/*record close operations*/,
                                 false/*record closed fields*/,
                                 new_states, already_open);
        // Remove the state if it is now empty
        if (!it->valid_fields)
          it = state.field_states.erase(it);
        else
          it++;
      }
      // Merge any new field states
      merge_new_field_states(state, new_states);
      // Record the version numbers that we need
      // If we're doing a read-only close, we don't need the version numbers
      if (!read_only_close)
        closer.record_version_numbers(this, state, 
                                      closing_mask, permit_leave_open);
      // If we're doing a close operation, that means someone is
      // going to be writing to a region that aliases with this one
      // so we need to advance the field version. However, if we're
      // staying open then we don't need to be advanced since we
      // will still be valid
      if (!permit_leave_open)
        state.advance_version_numbers(closing_mask);
      // We can also mark that there is no longer any dirty data below
      state.dirty_below -= closing_mask;
      // These fields are now fully closed
      if (!!state.partially_closed)
        state.partially_closed -= closing_mask;
      // We can also clear any outstanding reduction fields
      if (!(state.outstanding_reduction_fields * closing_mask))
        clear_logical_reduction_fields(state, closing_mask);
#ifdef DEBUG_LEGION
      sanity_check_logical_state(state);
#endif
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::siphon_logical_children(LogicalCloser &closer,
                                                 CurrentState &state,
                                                 const FieldMask &current_mask,
                                                 bool record_close_operations,
                                                 const ColorPoint &next_child,
                                                 FieldMask &open_below)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, 
                        REGION_NODE_SIPHON_LOGICAL_CHILDREN_CALL);
#ifdef DEBUG_LEGION
      sanity_check_logical_state(state);
#endif
      LegionDeque<FieldState>::aligned new_states;
      // Before looking at any child states, first check to see if we need
      // to do any closes to flush open reductions. This should be a pretty
      // rare operation since we often won't have lots of reductions going
      // on at different levels of the region tree.
      if (!!state.outstanding_reduction_fields)
      {
        FieldMask reduction_flush_fields = 
          current_mask & state.outstanding_reduction_fields;
        if (!!reduction_flush_fields)
        {
          // If we are doing a reduction too, check to see if they are 
          // the same in which case we can skip these fields
          if (closer.user.usage.redop > 0)
          {
            LegionMap<ReductionOpID,FieldMask>::aligned::const_iterator finder =
              state.outstanding_reductions.find(closer.user.usage.redop);
            // Don't need to flush fields we are reducing to with the
            // same operation
            if (finder != state.outstanding_reductions.end())
              reduction_flush_fields -= finder->second;
          }
          // See if we still have fields to close
          if (!!reduction_flush_fields)
          {
            FieldMask flushed_fields;
            // We need to flush these fields so issue close operations
            for (std::list<FieldState>::iterator it = 
                  state.field_states.begin(); it != 
                  state.field_states.end(); /*nothing*/)
            {
              FieldMask overlap = it->valid_fields & reduction_flush_fields;
              if (!overlap)
              {
                it++;
                continue;
              }
              FieldMask closed_child_fields;
              perform_close_operations(closer, overlap, *it,
                                       next_child, false/*allow_next*/,
                                       false/*needs upgrade*/,
                                       false/*permit leave open*/,
                                       false/*read only close*/,
                                       record_close_operations,
                                       true/*record closed fields*/,
                                       new_states, closed_child_fields);
              // We only really flushed fields that were actually closed
              flushed_fields |= closed_child_fields;
              if (!it->valid_fields)
                it = state.field_states.erase(it);
              else
                it++;
            }
            // Check to see if we have any unflushed fields
            // These are fields which still need a close operation
            // to be performed but only to flush the reductions
            FieldMask unflushed = reduction_flush_fields - flushed_fields;
            if (!!unflushed)
              closer.record_flush_only_fields(unflushed);
            // Then we can mark that these fields no longer have 
            // unflushed reductions
            clear_logical_reduction_fields(state, reduction_flush_fields);
          }
        }
      }
      // Now we can look at all the children
      for (LegionList<FieldState>::aligned::iterator it = 
            state.field_states.begin(); it != 
            state.field_states.end(); /*nothing*/)
      {
        // Quick check for disjointness, in which case we can continue
        if (it->valid_fields * current_mask)
        {
          it++;
          continue;
        }
        // Now check the current state
        switch (it->open_state)
        {
          case OPEN_READ_ONLY:
            {
              if (IS_READ_ONLY(closer.user.usage))
              {
                // Everything is read-only
                // See if the child that we want is already open
                if (next_child.is_valid())
                {
                  LegionMap<ColorPoint,FieldMask>::aligned::const_iterator 
                    finder = it->open_children.find(next_child);
                  if (finder != it->open_children.end())
                  {
                    // Remove the child's open fields from the
                    // list of fields we need to open
                    open_below |= finder->second;
                  }
                }
                it++;
              }
              else
              {
                // Not read-only
                // Close up all the open partitions except the one
                // we want to go down, make a new state to be added
                // containing the fields that are still open and mark
                // that we need an upgrade from read-only to some
                // kind of write operation. Note that we don't need to
                // actually perform close operations here because closing
                // read-only children requires no work.
                const bool needs_upgrade = HAS_WRITE(closer.user.usage);
                FieldMask already_open;
                perform_close_operations(closer, current_mask, *it, next_child,
                                         true/*allow next*/,
                                         needs_upgrade,
                                         false/*permit leave open*/,
                                         true/*read only close*/,
                                         record_close_operations,
                                         false/*record closed fields*/,
                                         new_states, already_open);
                open_below |= already_open;
                if (needs_upgrade && !!already_open)
                  new_states.push_back(
                      FieldState(closer.user, already_open, next_child));
                // See if there are still any valid open fields
                if (!it->valid_fields)
                  it = state.field_states.erase(it);
                else
                  it++;
              }
              break;
            }
          case OPEN_READ_WRITE:
            {
              // Close up any open partitions that conflict with ours
              perform_close_operations(closer, current_mask, *it, next_child,
                                       true/*allow next*/,
                                       false/*needs upgrade*/,
                                       IS_READ_ONLY(closer.user.usage),
                                       false/*read only close*/,
                                       record_close_operations,
                                       false/*record closed fields*/,
                                       new_states, open_below);
              if (!it->valid_fields)
                it = state.field_states.erase(it);
              else
                it++;
              break;
            }
          case OPEN_SINGLE_REDUCE:
            {
              // Check to see if we have a child we want to go down
              if (next_child.is_valid())
              {
                // There are four cases here:
                //   1. Same reduction, same child -> everything stays the same
                //   2. Same reduction, different child -> go to MULTI_REDUCE
                //   3. Diff operation, same child -> go to READ_WRITE
                //   4. Diff operation, diff child -> close everything up
                if (IS_REDUCE(closer.user.usage) && 
                    (it->redop == closer.user.usage.redop))
                {
                  // Cases 1 and 2
                  bool needs_recompute = false;
                  std::vector<ColorPoint> to_delete;
                  // Go through all the children and see if there is any overlap
                  for (LegionMap<ColorPoint,FieldMask>::aligned::iterator 
                        cit = it->open_children.begin(); cit !=
                        it->open_children.end(); cit++)
                  {
                    FieldMask already_open = cit->second & current_mask;
                    // If disjoint children, nothing to do
                    if (!already_open || 
                        are_children_disjoint(cit->first, next_child))
                      continue;
                    // Add the already open fields to this open_below mask
                    // since either they are already open for the right child
                    // or we're going to mark them open in a new FieldState
                    open_below |= already_open;
                    // Case 2
                    if (cit->first != (next_child))
                    {
                      // Different child so we need to create a new
                      // FieldState in MULTI_REDUCE mode with two
                      // children open
                      FieldState new_state(closer.user,already_open,cit->first);
                      // Add the next child as well
                      new_state.open_children[next_child] = 
                        already_open;
                      new_state.open_state = OPEN_MULTI_REDUCE;
#ifdef DEBUG_LEGION
                      assert(!!new_state.valid_fields);
#endif
                      new_states.push_back(new_state);
                      // Update the current child, mark that we need to
                      // recompute the valid fields for the state
                      cit->second -= already_open;
                      if (!cit->second)
                        to_delete.push_back(cit->first);
                      needs_recompute = true;
                    }
                    // Otherwise same child so case 1 and everything just
                    // stays in SINGLE_REDUCE_MODE
                  }
                  // See if we need to recompute any properties
                  // of the current state to see if they are still valid
                  if (needs_recompute)
                  {
                    // Remove all the empty children
                    for (std::vector<ColorPoint>::const_iterator cit = 
                          to_delete.begin(); cit != to_delete.end(); cit++)
                    {
                      LegionMap<ColorPoint,FieldMask>::aligned::iterator 
                        finder = it->open_children.find(*cit);
#ifdef DEBUG_LEGION
                      assert(finder != it->open_children.end());
                      assert(!finder->second);
#endif
                      it->open_children.erase(finder);
                    }
                    // Then recompute the valid mask for the current state
                    FieldMask new_valid_mask;
                    for (LegionMap<ColorPoint,FieldMask>::aligned::
                          const_iterator cit = it->open_children.begin(); 
                          cit != it->open_children.end(); cit++)
                    {
#ifdef DEBUG_LEGION
                      assert(!!cit->second);
#endif
                      new_valid_mask |= cit->second;
                    }
                    // Update the valid mask on the field state, we'll
                    // check to see fi we need to delete it at the end
                    it->valid_fields = new_valid_mask;
                  }
                }
                else
                {
                  // Cases 3 and 4
                  FieldMask already_open;
                  perform_close_operations(closer, current_mask, *it, 
                                           next_child, 
                                           true/*allow next*/,
                                           true/*needs upgrade*/,
                                           false/*permit leave open*/,
                                           false/*read only close*/,
                                           record_close_operations,
                                           false/*record closed fields*/,
                                           new_states, already_open);
                  open_below |= already_open;
                  if (!!already_open)
                  {
                    // Create a new FieldState open in whatever mode is
                    // appropriate based on the usage
                    FieldState new_state(closer.user, already_open, next_child);
                    // We always have to go to read-write mode here
                    new_state.open_state = OPEN_READ_WRITE;
                    new_states.push_back(new_state);
                  }
                }
              }
              else
              {
                // Closing everything up, so just do it
                FieldMask already_open;
                perform_close_operations(closer, current_mask, *it, next_child,
                                         false/*allow next*/,
                                         false/*needs upgrade*/,
                                         false/*permit leave open*/,
                                         false/*read only close*/,
                                         record_close_operations,
                                         false/*record closed fields*/,
                                         new_states, already_open);
                open_below |= already_open;
              }
              // Now see if the current field state is still valid
              if (!it->valid_fields)
                it = state.field_states.erase(it);
              else
                it++;
              break;
            }
          case OPEN_MULTI_REDUCE:
            {
              // See if this reduction is a reduction of the same kind
              if (IS_REDUCE(closer.user.usage) &&
                  (closer.user.usage.redop == it->redop))
              {
                if (next_child.is_valid())
                {
                  LegionMap<ColorPoint,FieldMask>::aligned::const_iterator
                    finder = it->open_children.find(next_child);
                  if (finder != it->open_children.end())
                  {
                    // Already open, so add the open fields
                    open_below |= (finder->second & current_mask);
                  }
                }
                it++;
              }
              else
              {
                // Need to close up the open field since we're going
                // to have to do it anyway
                FieldMask already_open;
                perform_close_operations(closer, current_mask, *it, next_child,
                                         false/*allow next child*/,
                                         false/*needs upgrade*/,
                                         false/*permit leave open*/,
                                         false/*read only close*/,
                                         record_close_operations,
                                         false/*record closed fields*/,
                                         new_states, already_open);
#ifdef DEBUG_LEGION
                assert(!already_open); // should all be closed now
#endif
                if (!it->valid_fields)
                  it = state.field_states.erase(it);
                else
                  it++;
              }
              break;
            }
          default:
            assert(false); // should never get here
        }
      }
      // If we had any fields that still need to be opened, create
      // a new field state and add it into the set of new states
      FieldMask open_mask = current_mask - open_below;
      if (next_child.is_valid() && !!open_mask)
        new_states.push_back(FieldState(closer.user, open_mask, next_child));
      merge_new_field_states(state, new_states);
#ifdef DEBUG_LEGION
      sanity_check_logical_state(state);
#endif 
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::perform_close_operations(LogicalCloser &closer,
                                            const FieldMask &closing_mask,
                                            FieldState &state,
                                            const ColorPoint &next_child, 
                                            bool allow_next_child,
                                            bool upgrade_next_child,
                                            bool permit_leave_open,
                                            bool read_only_close,
                                            bool record_close_operations,
                                            bool record_closed_fields,
                                   LegionDeque<FieldState>::aligned &new_states,
                                            FieldMask &output_mask)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, 
                        REGION_NODE_PERFORM_LOGICAL_CLOSES_CALL);
#ifdef DEBUG_LEGION
      // These two things have to be mutually exclusive because
      // they cannot both share the output_mask
      assert(!allow_next_child || !record_closed_fields);
#endif
      // First, if we have a next child and we know all pairs of children
      // are disjoint, then we can skip a lot of this
      bool removed_fields = false;
      if (next_child.is_valid() && are_all_children_disjoint())
      {
        bool performed_close = false;
        // Check to see if we have anything to close
        LegionMap<ColorPoint,FieldMask>::aligned::iterator finder = 
                              state.open_children.find(next_child);
        if (finder != state.open_children.end())
        {
          FieldMask close_mask = finder->second & closing_mask;
          if (!!close_mask)
          {
            if (allow_next_child)
            {
              output_mask |= close_mask;
              if (upgrade_next_child)
              {
                finder->second -= close_mask;
                removed_fields = true;
                if (!finder->second)
                  state.open_children.erase(finder);
              }
            }
            else
            {
              // Otherwise we actually need to do the close
              RegionTreeNode *child_node = get_tree_child(finder->first);
              child_node->close_logical_node(closer, close_mask, 
                                             permit_leave_open, 
                                             read_only_close);
              if (record_close_operations)
              {
                closer.record_closed_child(finder->first, close_mask, 
                                           permit_leave_open, read_only_close);
                performed_close = true;
              }
              // Remove the closed fields
              finder->second -= close_mask;
              removed_fields = true;
              if (permit_leave_open)
              {
                new_states.push_back(FieldState(closer.user,
                                  close_mask, finder->first));
              }
              if (!finder->second)
                state.open_children.erase(finder);
              // Record the closed fields if necessary
              if (record_closed_fields)
                output_mask |= close_mask;
            }
          }
          // Otherwise disjoint fields, nothing to do
        }
        // Otherwise it's closed so it doesn't matter

        // If we did the close, see if this is the
        // first partial close for any fields
        if (performed_close)
        {
          FieldMask remaining = closing_mask;
          for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it = 
                state.open_children.begin(); it != 
                state.open_children.end(); it++)
          {
            if (it->first == next_child)
              continue;
            FieldMask overlap = remaining & it->second;
            if (!overlap)
              continue;
            closer.record_partial_fields(overlap);
            remaining -= overlap;
            // If there are no more fields to check we are done
            if (!remaining)
              continue;
          }
        }
      }
      else
      {
        std::vector<ColorPoint> to_delete;
        // Go through and close all the children which we overlap with
        // and aren't the next child that we're going to use
        for (LegionMap<ColorPoint,FieldMask>::aligned::iterator it = 
              state.open_children.begin(); it != 
              state.open_children.end(); it++)
        {
          FieldMask close_mask = it->second & closing_mask;
          // check for field disjointness
          if (!close_mask)
            continue;
          // Check for same child, only allow upgrades in some cases
          // such as read-only -> exclusive.  This is calling context
          // sensitive hence the parameter.
          if (allow_next_child && next_child.is_valid() && 
              ((next_child) == it->first))
          {
            FieldMask open_fields = close_mask;
            output_mask |= open_fields;
            if (upgrade_next_child)
            {
              it->second -= open_fields;
              removed_fields = true;
              if (!it->second)
                to_delete.push_back(it->first);
              // The upgraded field state gets added by the caller
            }
            continue;
          }
          // Check for child disjointness
          if (next_child.is_valid() && 
              are_children_disjoint(it->first, next_child))
          {
            // If we're recording, note that we are about
            // to do a partial close
            if (record_close_operations)
              closer.record_partial_fields(close_mask);
            continue;
          }
          // Perform the close operation
          RegionTreeNode *child_node = get_tree_child(it->first);
          child_node->close_logical_node(closer, close_mask, 
                                         permit_leave_open, read_only_close);
          if (record_close_operations)
            closer.record_closed_child(it->first, close_mask, 
                                       permit_leave_open, read_only_close);
          // Remove the close fields
          it->second -= close_mask;
          removed_fields = true;
          if (!it->second)
            to_delete.push_back(it->first);
          // If we're allowed to leave this open, add a new
          // state for the current user
          if (permit_leave_open)
          {
            new_states.push_back(FieldState(closer.user,close_mask,it->first));
          }
          if (record_closed_fields)
            output_mask |= close_mask;
        }
        // Remove the children that can be deleted
        for (std::vector<ColorPoint>::const_iterator it = to_delete.begin();
              it != to_delete.end(); it++)
        {
          state.open_children.erase(*it);
        }
      }
      // See if it is time to rebuild the valid mask 
      if (removed_fields)
      {
        if (state.rebuild_timeout == 0)
        {
          // Rebuild the valid fields mask
          FieldMask new_valid_mask;
          for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it = 
                state.open_children.begin(); it != 
                state.open_children.end(); it++)
          {
            new_valid_mask |= it->second;
          }
          state.valid_fields = new_valid_mask;    
          // Reset the timeout to the order of the number of open children
          state.rebuild_timeout = state.open_children.size();
        }
        else
          state.rebuild_timeout--;
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::merge_new_field_state(CurrentState &state,
                                               const FieldState &new_state)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!!new_state.valid_fields);
#endif
      for (std::list<FieldState>::iterator it = state.field_states.begin();
            it != state.field_states.end(); it++)
      {
        if (it->overlaps(new_state))
        {
          it->merge(new_state, this);
          return;
        }
      }
      // Otherwise just push it on the back
      state.field_states.push_back(new_state);
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::merge_new_field_states(CurrentState &state,
                             const LegionDeque<FieldState>::aligned &new_states)
    //--------------------------------------------------------------------------
    {
      for (unsigned idx = 0; idx < new_states.size(); idx++)
      {
        const FieldState &next = new_states[idx];
        merge_new_field_state(state, next);
      }
#ifdef DEBUG_LEGION
      sanity_check_logical_state(state);
#endif
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::filter_prev_epoch_users(CurrentState &state,
                                                 const FieldMask &field_mask)
    //--------------------------------------------------------------------------
    {
      for (LegionList<LogicalUser,PREV_LOGICAL_ALLOC>::track_aligned::iterator 
            it = state.prev_epoch_users.begin(); it != 
            state.prev_epoch_users.end(); /*nothing*/)
      {
        it->field_mask -= field_mask;
        if (!it->field_mask)
        {
          // Remove the mapping reference
          it->op->remove_mapping_reference(it->gen);
          it = state.prev_epoch_users.erase(it); // empty so erase it
        }
        else
          it++; // still has non-dominated fields
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::filter_curr_epoch_users(CurrentState &state,
                                                 const FieldMask &field_mask)
    //--------------------------------------------------------------------------
    {
      for (LegionList<LogicalUser,CURR_LOGICAL_ALLOC>::track_aligned::iterator 
            it = state.curr_epoch_users.begin(); it !=
            state.curr_epoch_users.end(); /*nothing*/)
      {
        FieldMask local_dom = it->field_mask & field_mask;
        if (!!local_dom)
        {
          // Move a copy over to the previous epoch users for
          // the fields that were dominated
          state.prev_epoch_users.push_back(*it);
          state.prev_epoch_users.back().field_mask = local_dom;
          // Add a mapping reference
          it->op->add_mapping_reference(it->gen);
        }
        else
        {
          it++;
          continue;
        }
        // Update the field mask with the non-dominated fields
        it->field_mask -= local_dom;
        if (!it->field_mask)
        {
          // Remove the mapping reference
          it->op->remove_mapping_reference(it->gen);
          it = state.curr_epoch_users.erase(it); // empty so erase it
        }
        else
          it++; // not empty so keep going
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::report_uninitialized_usage(const LogicalUser &user,
                                                    const FieldMask &uninit)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_region());
#endif
      LogicalRegion handle = as_region_node()->handle;
      char *field_string = column_source->to_string(uninit);
      log_run.warning("WARNING: Region requirement %d of operation %s "
                      "(UID %lld) is using uninitialized data for field(s) %s "
                      "of logical region (%d,%d,%d)", user.idx, 
                      user.op->get_logging_name(), user.op->get_unique_op_id(),
                      field_string, handle.get_index_space().get_id(),
                      handle.get_field_space().get_id(), 
                      handle.get_tree_id());
      free(field_string);
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::record_logical_reduction(CurrentState &state,
                                                  ReductionOpID redop,
                                                  const FieldMask &user_mask)
    //--------------------------------------------------------------------------
    {
      state.outstanding_reduction_fields |= user_mask;
      LegionMap<ReductionOpID,FieldMask>::aligned::iterator finder = 
        state.outstanding_reductions.find(redop);
      if (finder == state.outstanding_reductions.end())
        state.outstanding_reductions[redop] = user_mask;
      else
        finder->second |= user_mask;
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::clear_logical_reduction_fields(CurrentState &state,
                                                  const FieldMask &cleared_mask)
    //--------------------------------------------------------------------------
    {
      state.outstanding_reduction_fields -= cleared_mask; 
      std::vector<ReductionOpID> to_delete;
      for (LegionMap<ReductionOpID,FieldMask>::aligned::iterator it = 
            state.outstanding_reductions.begin(); it !=
            state.outstanding_reductions.end(); it++)
      {
        it->second -= cleared_mask; 
        if (!it->second)
          to_delete.push_back(it->first);
      }
      for (std::vector<ReductionOpID>::const_iterator it = 
            to_delete.begin(); it != to_delete.end(); it++)
      {
        state.outstanding_reductions.erase(*it);
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::sanity_check_logical_state(CurrentState &state)
    //--------------------------------------------------------------------------
    {
      // For every child and every field, it should only be open in one mode
      LegionMap<ColorPoint,FieldMask>::aligned previous_children;
      for (std::list<FieldState>::const_iterator fit = 
            state.field_states.begin(); fit != 
            state.field_states.end(); fit++)
      {
        FieldMask actually_valid;
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it =
              fit->open_children.begin(); it != 
              fit->open_children.end(); it++)
        {
          actually_valid |= it->second;
          if (previous_children.find(it->first) == previous_children.end())
          {
            previous_children[it->first] = it->second;
          }
          else
          {
            FieldMask &previous = previous_children[it->first];
            assert(!(previous & it->second));
            previous |= it->second;
          }
        }
        // Actually valid should be greater than or equal
        assert(!(actually_valid - fit->valid_fields));
      }
      // Also check that for each field it is either only open in one mode
      // or two children in different modes are disjoint
      for (std::list<FieldState>::const_iterator it1 = 
            state.field_states.begin(); it1 != 
            state.field_states.end(); it1++)
      {
        for (std::list<FieldState>::const_iterator it2 = 
              state.field_states.begin(); it2 != 
              state.field_states.end(); it2++)
        {
          // No need to do comparisons if they are the same field state
          if (it1 == it2) 
            continue;
          const FieldState &f1 = *it1;
          const FieldState &f2 = *it2;
          for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator 
                cit1 = f1.open_children.begin(); cit1 != 
                f1.open_children.end(); cit1++)
          {
            for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator 
                  cit2 = f2.open_children.begin(); 
                  cit2 != f2.open_children.end(); cit2++)
            {
              
              // Disjointness check on fields
              if (cit1->second * cit2->second)
                continue;
#ifndef NDEBUG
              ColorPoint c1 = cit1->first;
              ColorPoint c2 = cit2->first;
#endif
              // Some aliasing in the fields, so do the check 
              // for child disjointness
              assert(c1 != c2);
              assert(are_children_disjoint(c1, c2));
            }
          }
        }
      }
      // Make sure each field appears in exactly one version number
      state.sanity_check();
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::register_logical_dependences(ContextID ctx, 
                      Operation *op, const FieldMask &field_mask, bool dominate)
    //--------------------------------------------------------------------------
    {
      CurrentState &state = get_current_state(ctx);
      for (LegionList<LogicalUser,CURR_LOGICAL_ALLOC>::track_aligned::iterator 
            it = state.curr_epoch_users.begin(); it != 
            state.curr_epoch_users.end(); /*nothing*/)
      {
        if (!(it->field_mask * field_mask))
        {
#ifdef LEGION_SPY
          LegionSpy::log_mapping_dependence(
              op->get_parent()->get_unique_id(),
              it->uid, it->idx, op->get_unique_op_id(),
              0/*idx*/, TRUE_DEPENDENCE);
#endif
          // Do this after the logging since we 
          // are going to update the iterator
          if (op->register_dependence(it->op, it->gen))
          {
#ifndef LEGION_SPY
            // Prune it from the list
            it = state.curr_epoch_users.erase(it);
#else
            it++;
#endif
          }
          else if (dominate)
            it = state.curr_epoch_users.erase(it);
          else
            it++;
        }
        else if (dominate)
          it = state.curr_epoch_users.erase(it);
        else
          it++;
      }
      for (LegionList<LogicalUser,PREV_LOGICAL_ALLOC>::track_aligned::iterator 
            it = state.prev_epoch_users.begin(); it != 
            state.prev_epoch_users.end(); /*nothing*/)
      {
        if (!(it->field_mask * field_mask))
        {
#ifdef LEGION_SPY
          LegionSpy::log_mapping_dependence(
              op->get_parent()->get_unique_id(),
              it->uid, it->idx, op->get_unique_op_id(), 0/*idx*/, 
              TRUE_DEPENDENCE);
#endif
          // Do this after the logging since we are going
          // to update the iterator
          if (op->register_dependence(it->op, it->gen))
          {
#ifndef LEGION_SPY
            // Prune it from the list
            it = state.prev_epoch_users.erase(it);
#else
            it++;
#endif
          }
          else if (dominate)
            it = state.prev_epoch_users.erase(it);
          else
            it++;
        }
        else if (dominate)
          it = state.prev_epoch_users.erase(it);
        else
          it++;
      }
    } 

    //--------------------------------------------------------------------------
    void RegionTreeNode::register_logical_deletion(ContextID ctx,
                                                   const LogicalUser &user,
                                                   const FieldMask &check_mask,
                                                   RegionTreePath &path,
                                                   VersionInfo &version_info,
                                                   RestrictInfo &restrict_info,
                                                   const TraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, 
                        REGION_NODE_REGISTER_LOGICAL_USER_CALL);
      CurrentState &state = get_current_state(ctx);
#ifdef DEBUG_LEGION
      sanity_check_logical_state(state);
#endif
      const unsigned depth = get_depth();
      const bool arrived = !path.has_child(depth);
      if (!arrived)
      {
        FieldMask open_below;
        ColorPoint next_child = path.get_child(depth);
        if (!!check_mask)
        {
          // Perform any close operations
          LogicalCloser closer(ctx, user, false/*validates*/, true/*capture*/);
          siphon_logical_deletion(closer, state, check_mask, next_child, 
                              open_below, ((depth+1) == path.get_max_depth()));
          if (closer.has_close_operations())
          {
            // Generate the close operations         
            // We need to record the version numbers for this node as well
            closer.record_top_version_numbers(this, state);
            closer.initialize_close_operations(this, user.op, 
                                               version_info, trace_info);
            if (!arrived)
              closer.add_next_child(next_child);
            // Perform dependence analysis for all the close operations
            closer.perform_dependence_analysis(user, open_below,
                                               state.curr_epoch_users,
                                               state.prev_epoch_users);
            // Note we don't need to update the version numbers because
            // that happened when we recorded dirty fields below. 
            // However, we do need to mark that there is no longer any
            // dirty data below this node for all the closed fields

            // Update the dirty_below and partial close fields
            // and filter the current and previous epochs
            closer.update_state(state);
            // Now we can add the close operations to the current epoch
            closer.register_close_operations(state.curr_epoch_users);
          }
          // Perform our checks on dependences
          FieldMask dominator_mask = 
                 perform_dependence_checks<CURR_LOGICAL_ALLOC,
                           true/*record*/,false/*has skip*/,true/*track dom*/>(
                              user, state.curr_epoch_users, check_mask, 
                              open_below, false/*validates*/);
          FieldMask non_dominated_mask = check_mask - dominator_mask;
          // For the fields that weren't dominated, we have to check
          // those fields against the previous epoch's users
          if (!!non_dominated_mask)
          {
            perform_dependence_checks<PREV_LOGICAL_ALLOC,
                         true/*record*/, false/*has skip*/, false/*track dom*/>(
                         user, state.prev_epoch_users, non_dominated_mask, 
                         open_below, false/*validates*/);
          }
          // No need to update the version numbers since we're not actually
          // mutating anything here, still need to record the version numbers
          // though in case we need to issue any close operations
          FieldMask dirty_overlap = 
            check_mask & (state.dirty_below - state.partially_closed);
          if (!dirty_overlap)
          {
            // No dirty fields below, which means we don't have any previous
            state.record_version_numbers(check_mask, user, version_info,
                                   false/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   false/*report uninitialized*/);
          }
          else
          {
            // We have overlapping dirty fields, capture previous
            // Record split node for readers!
            state.record_version_numbers(dirty_overlap, user, version_info,
                                   true/*previous*/, true/*path only*/,
                                   false/*final*/, false/*close top*/,
                                   false/*report uninitialized*/);
            // See if we have any non-overlapping
            FieldMask non_dirty = check_mask - dirty_overlap;
            if (!!non_dirty)
              state.record_version_numbers(non_dirty, user, version_info,
                                     false/*previous*/, true/*path only*/,
                                     false/*final*/, false/*close top*/,
                                     false/*report uninitialized*/);
          }
        }
        // Continue the traversal
        RegionTreeNode *child = get_tree_child(next_child);
        // Only continue checking the fields that are open below
        child->register_logical_deletion(ctx, user, open_below, path,
                                       version_info, restrict_info, trace_info);
      }
      else
      {
        // Register dependences on any users in the sub-tree
        if (!!check_mask)
        {
          // Don't dominate so later deletions with overlapping region
          // requirements can catch the same dependences
          LogicalRegistrar registrar(ctx, user.op, 
                                     check_mask, false/*dominate*/);
          visit_node(&registrar);
        }
        // We've arrived, so clear out the current state here and anywhere below
        DeletionInvalidator invalidator(ctx, user.field_mask);
        visit_node(&invalidator);
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::siphon_logical_deletion(LogicalCloser &closer,
                                                 CurrentState &state,
                                                 const FieldMask &current_mask,
                                                 const ColorPoint &next_child,
                                                 FieldMask &open_below,
                                                 bool force_close_next)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(next_child.is_valid());
      sanity_check_logical_state(state);
#endif
      LegionDeque<FieldState>::aligned new_states;
      for (LegionList<FieldState>::aligned::iterator it = 
            state.field_states.begin(); it != 
            state.field_states.end(); /*nothing*/)
      {
        // Quick check for disjointness, in which case we can continue
        if (it->valid_fields * current_mask)
        {
          it++;
          continue;
        }
        // See if our child is open, if it's not then we can keep going
        LegionMap<ColorPoint,FieldMask>::aligned::iterator finder = 
          it->open_children.find(next_child);
        if (finder == it->open_children.end())
        {
          it++;
          continue;
        }
        const FieldMask overlap = finder->second & current_mask;
        if (!overlap)
        {
          it++;
          continue;
        }
        // Now check the current state
        switch (it->open_state)
        {
          case OPEN_READ_ONLY:
            {
              finder->second -= overlap;
              if (!finder->second)
                it->open_children.erase(finder);
              // Rebuild the state mask
              FieldMask new_state_mask;
              for (LegionMap<ColorPoint,FieldMask>::aligned::iterator
                    child_it = it->open_children.begin(); child_it !=
                    it->open_children.end(); child_it++)
                new_state_mask |= child_it->second;
              if (!!new_state_mask)
              {
                it->valid_fields = new_state_mask;
                it++;
              }
              else
                it = state.field_states.erase(it);
              break;
            }
          case OPEN_READ_WRITE:
            {
              // See if we need to force the close, otherwise we 
              // can record that it is already open
              if (force_close_next)
              {
                perform_close_operations(closer, current_mask, *it,
                                         next_child, false/*allow next*/,
                                         false/*upgrade next child*/,
                                         false/*permit leave open*/,
                                         false/*read only close*/,
                                         true/*record close operations*/,
                                         true/*record closed fields*/,
                                         new_states, open_below);
                if (!it->valid_fields)
                  it = state.field_states.erase(it);
                else
                  it++;
              }
              else
              {
                open_below |= (finder->second & current_mask);
                it++;
              }
              break;
            }
          case OPEN_SINGLE_REDUCE:
            {
              // Go to read-write mode and close if necessary
              if (force_close_next)
              {
                perform_close_operations(closer, current_mask, *it,
                                         next_child, false/*allow next*/,
                                         false/*upgrade next child*/,
                                         false/*permit leave open*/,
                                         false/*read only close*/,
                                         true/*record close operations*/,
                                         true/*record closed fields*/,
                                         new_states, open_below);
                if (!it->valid_fields)
                  it = state.field_states.erase(it);
                else
                  it++;
              }
              else
              {
                open_below |= overlap;
                finder->second -= overlap;
                // Add the new read-write state
                new_states.push_back(
                    FieldState(closer.user, overlap, next_child));
                if (!finder->second)
                  it->open_children.erase(finder);  
                // Rebuild the state mask
                FieldMask new_state_mask;
                for (LegionMap<ColorPoint,FieldMask>::aligned::iterator
                      child_it = it->open_children.begin(); child_it !=
                      it->open_children.end(); child_it++)
                  new_state_mask |= child_it->second;
                if (!!new_state_mask)
                {
                  it->valid_fields = new_state_mask;
                  it++;
                }
                else
                  it = state.field_states.erase(it);
              }
              break;
            }
          case OPEN_MULTI_REDUCE:
            {
              // Do the close here if our child is open
              perform_close_operations(closer, current_mask, *it,
                                       next_child, false/*allow next*/,
                                       false/*upgrade next child*/,
                                       false/*permit leave open*/,
                                       false/*read only close*/,
                                       true/*record close operations*/,
                                       true/*record closed fields*/,
                                       new_states, open_below);
              if (!it->valid_fields)
                it = state.field_states.erase(it);
              else
                it++;
              break;
            }
          default:
            assert(false); // should never get here
        }
      }
      merge_new_field_states(state, new_states);
#ifdef DEBUG_LEGION
      sanity_check_logical_state(state);
#endif
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::send_back_logical_state(ContextID local_ctx,
                                                 ContextID remote_ctx,
                                                 const FieldMask &send_mask,
                                                 AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      CurrentState &state = get_current_state(local_ctx);
      Serializer field_rez;
      unsigned num_field_states = 0;
      for (LegionList<FieldState>::aligned::const_iterator it = 
            state.field_states.begin(); it != state.field_states.end(); it++)
      {
        if (send_mask * it->valid_fields)
          continue;
        num_field_states++;
        field_rez.serialize(it->open_state);
        field_rez.serialize(it->redop);
        unsigned num_children = 0;
        Serializer child_rez;
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator cit = 
              it->open_children.begin(); cit != it->open_children.end(); cit++)
        {
          FieldMask overlap = send_mask & cit->second;
          if (!overlap)
            continue;
          num_children++;
          child_rez.serialize(cit->first);
          child_rez.serialize(overlap);
          // Also traverse the child
          RegionTreeNode *child = get_tree_child(cit->first);
          child->send_back_logical_state(local_ctx, remote_ctx, overlap,target);
        }
        field_rez.serialize(num_children);
        field_rez.serialize(child_rez.get_buffer(), child_rez.get_used_bytes());
      }
      // Make an event to indicate when we are done moving the state back
      RtUserEvent done_event = Runtime::create_rt_user_event();
      Serializer rez;
      {
        RezCheck z(rez); 
        if (is_region())
        {
          rez.serialize<bool>(true);
          rez.serialize(as_region_node()->handle);
        }
        else
        {
          rez.serialize<bool>(false);
          rez.serialize(as_partition_node()->handle);
        }
        rez.serialize(remote_ctx);
        rez.serialize(num_field_states);
        rez.serialize(field_rez.get_buffer(), field_rez.get_used_bytes());
        // No need to pack the users, they are done
        Serializer current_rez;
        unsigned num_current = 0;
        for (LegionMap<VersionID,VersionStateInfo>::aligned::const_iterator
              vit = state.current_version_infos.begin(); 
              vit != state.current_version_infos.end(); vit++)
        {
          const VersionStateInfo &info = vit->second; 
          if (info.single)
          {
            FieldMask overlap = info.valid_fields & send_mask;
            if (!overlap)
              continue;
            num_current++;
            current_rez.serialize(vit->first);
            current_rez.serialize<unsigned>(1);
            // Add a valid reference for when it is in transit
            info.states.single_state->add_base_valid_ref(REMOTE_DID_REF);
            // Remove it pending the completion of the transfer
            info.states.single_state->remove_version_state_ref(REMOTE_DID_REF,
                                                               done_event);
            current_rez.serialize(info.states.single_state->did);
            current_rez.serialize(info.states.single_state->owner_space);
            current_rez.serialize(overlap); 
          }
          else
          {
            if (info.valid_fields * send_mask)
              continue;
            num_current++;
            current_rez.serialize(vit->first);
            Serializer state_rez;
            unsigned num_states = 0;
            for (LegionMap<VersionState*,FieldMask>::aligned::const_iterator 
                  it = info.states.multi_states->begin(); it != 
                  info.states.multi_states->end(); it++)
            {
              FieldMask overlap = it->second & send_mask; 
              if (!overlap)
                continue;
              num_states++;
              // Add a valid reference for when it is in transit
              it->first->add_base_valid_ref(REMOTE_DID_REF);
              // Remove it pending the completion of the transfer 
              it->first->remove_version_state_ref(REMOTE_DID_REF, done_event);
              state_rez.serialize(it->first->did);
              state_rez.serialize(it->first->owner_space);
              state_rez.serialize(overlap);
            }
            current_rez.serialize(num_states);
            current_rez.serialize(state_rez.get_buffer(), 
                                  state_rez.get_used_bytes());
          }
        }
        rez.serialize(num_current);
        rez.serialize(current_rez.get_buffer(), current_rez.get_used_bytes());
        Serializer previous_rez;
        unsigned num_previous = 0;
        for (LegionMap<VersionID,VersionStateInfo>::aligned::const_iterator
              vit = state.previous_version_infos.begin(); 
              vit != state.previous_version_infos.end(); vit++)
        {
          const VersionStateInfo &info = vit->second; 
          if (info.single)
          {
            FieldMask overlap = info.valid_fields & send_mask;
            if (!overlap)
              continue;
            num_previous++;
            previous_rez.serialize(vit->first);
            previous_rez.serialize<unsigned>(1);
            // Add a valid reference for when it is in transit
            info.states.single_state->add_base_valid_ref(REMOTE_DID_REF);
            // Remove it pending the completion of the transfer 
            info.states.single_state->remove_version_state_ref(REMOTE_DID_REF,
                                                               done_event);
            previous_rez.serialize(info.states.single_state->did);
            previous_rez.serialize(info.states.single_state->owner_space);
            previous_rez.serialize(overlap);
          }
          else
          {
            if (info.valid_fields * send_mask)
              continue;
            num_previous++;
            previous_rez.serialize(vit->first);
            Serializer state_rez;
            unsigned num_states = 0;
            for (LegionMap<VersionState*,FieldMask>::aligned::const_iterator 
                  it = info.states.multi_states->begin(); it != 
                  info.states.multi_states->end(); it++)
            {
              FieldMask overlap = it->second & send_mask; 
              if (!overlap)
                continue;
              num_states++;
              // Add a valid reference for when it is in transit
              it->first->add_base_valid_ref(REMOTE_DID_REF);
              // Remove it pending the completion of the transfer 
              it->first->remove_version_state_ref(REMOTE_DID_REF, done_event);
              state_rez.serialize(it->first->did);
              state_rez.serialize(it->first->owner_space);
              state_rez.serialize(overlap);
            }
            previous_rez.serialize(num_states);
            previous_rez.serialize(state_rez.get_buffer(), 
                                   state_rez.get_used_bytes());
          }
        }
        rez.serialize(num_previous);
        rez.serialize(previous_rez.get_buffer(), previous_rez.get_used_bytes());
        if (!(state.outstanding_reduction_fields * send_mask))
        {
          Serializer reduc_rez;
          unsigned num_reductions = 0;
          for (LegionMap<ReductionOpID,FieldMask>::aligned::const_iterator it = 
                state.outstanding_reductions.begin(); it !=
                state.outstanding_reductions.end(); it++)
          {
            FieldMask overlap = it->second & send_mask;
            if (!overlap)
              continue;
            num_reductions++;
            reduc_rez.serialize(it->first);
            reduc_rez.serialize(overlap);
          }
          rez.serialize(num_reductions);
          rez.serialize(reduc_rez.get_buffer(), reduc_rez.get_used_bytes());
        }
        else
          rez.serialize<unsigned>(0);
        FieldMask send_dirty = state.dirty_below & send_mask;
        rez.serialize(send_dirty);
        FieldMask send_partial = state.partially_closed & send_mask;
        rez.serialize(send_partial);
        
        rez.serialize(done_event);
      }
      context->runtime->send_back_logical_state(target, rez);
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::process_logical_state_return(Deserializer &derez,
                               AddressSpaceID source, ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
      ContextID local_ctx;
      derez.deserialize(local_ctx);
      CurrentState &state = get_current_state(local_ctx);
      unsigned num_field_states;
      derez.deserialize(num_field_states);
      for (unsigned idx = 0; idx < num_field_states; idx++)
      {
        FieldState field_state;
        derez.deserialize(field_state.open_state);
        derez.deserialize(field_state.redop);
        unsigned num_children;
        derez.deserialize(num_children);
        if (num_children == 0)
          continue;
        for (unsigned idx2 = 0; idx2 < num_children; idx2++)
        {
          ColorPoint child;
          derez.deserialize(child);
          FieldMask &child_mask = field_state.open_children[child];
          derez.deserialize(child_mask);
          field_state.valid_fields |= child_mask;
        }
        merge_new_field_state(state, field_state);
      }
      unsigned num_current;
      derez.deserialize(num_current);
      for (unsigned idx = 0; idx < num_current; idx++)
      {
        VersionID vid;
        derez.deserialize(vid);
        VersionStateInfo &info = state.current_version_infos[vid];
        unsigned num_states;
        derez.deserialize(num_states);
        for (unsigned idx2 = 0; idx2 < num_states; idx2++)
        {
          DistributedID did;
          derez.deserialize(did);
          AddressSpaceID owner;
          derez.deserialize(owner);
          FieldMask state_mask;
          derez.deserialize(state_mask);
          VersionState *version_state = 
            find_remote_version_state(vid, did, owner, mutator);
          if (info.single)
          {
            if (info.states.single_state == NULL)
            {
              info.states.single_state = version_state;
              version_state->add_base_valid_ref(CURRENT_STATE_REF, mutator);
              info.valid_fields = state_mask;
            }
            else if (info.states.single_state == version_state)
            {
              info.valid_fields |= state_mask;
            }
            else
            {
              // go to multi
              LegionMap<VersionState*,FieldMask>::aligned *multi = 
                new LegionMap<VersionState*,FieldMask>::aligned();
              (*multi)[info.states.single_state] = info.valid_fields;
              (*multi)[version_state] = state_mask;
              version_state->add_base_valid_ref(CURRENT_STATE_REF, mutator);
              info.single = false;
              info.states.multi_states = multi;
              info.valid_fields |= state_mask;
            }
          }
          else
          {
            LegionMap<VersionState*,FieldMask>::aligned::iterator finder =
              info.states.multi_states->find(version_state);
            if (finder == info.states.multi_states->end())
            {
              (*info.states.multi_states)[version_state] = state_mask;
              version_state->add_base_valid_ref(CURRENT_STATE_REF, mutator);
            }
            else
              finder->second |= state_mask;
            info.valid_fields |= state_mask;
          }
        }
      }
      unsigned num_previous;
      derez.deserialize(num_previous);
      for (unsigned idx = 0; idx < num_previous; idx++)
      {
        VersionID vid;
        derez.deserialize(vid);
        VersionStateInfo &info = state.previous_version_infos[vid];
        unsigned num_states;
        derez.deserialize(num_states);
        for (unsigned idx2 = 0; idx2 < num_states; idx2++)
        {
          DistributedID did;
          derez.deserialize(did);
          AddressSpaceID owner;
          derez.deserialize(owner);
          FieldMask state_mask;
          derez.deserialize(state_mask);
          VersionState *version_state = 
            find_remote_version_state(vid, did, owner, mutator);
          if (info.single)
          {
            if (info.states.single_state == NULL)
            {
              info.states.single_state = version_state;
              version_state->add_base_valid_ref(CURRENT_STATE_REF, mutator);
              info.valid_fields = state_mask;
            }
            else if (info.states.single_state == version_state)
            {
              info.valid_fields |= state_mask;
            }
            else
            {
              // go to multi
              LegionMap<VersionState*,FieldMask>::aligned *multi = 
                new LegionMap<VersionState*,FieldMask>::aligned();
              (*multi)[info.states.single_state] = info.valid_fields;
              (*multi)[version_state] = state_mask;
              version_state->add_base_valid_ref(CURRENT_STATE_REF, mutator);
              info.single = false;
              info.states.multi_states = multi;
              info.valid_fields |= state_mask;
            }
          }
          else
          {
            LegionMap<VersionState*,FieldMask>::aligned::iterator finder = 
              info.states.multi_states->find(version_state);
            if (finder == info.states.multi_states->end())
            {
              (*info.states.multi_states)[version_state] = state_mask;
              version_state->add_base_valid_ref(CURRENT_STATE_REF, mutator);
            }
            else
              finder->second |= state_mask;
            info.valid_fields |= state_mask;
          }
        }
      }
      unsigned num_reduc;
      derez.deserialize(num_reduc);
      for (unsigned idx = 0; idx < num_reduc; idx++)
      {
        ReductionOpID redop;
        derez.deserialize(redop);
        FieldMask reduc_mask;
        derez.deserialize(reduc_mask);
        state.outstanding_reductions[redop] |= reduc_mask;
        state.outstanding_reduction_fields |= reduc_mask;
      }
      FieldMask dirty_below;
      derez.deserialize(dirty_below);
      state.dirty_below |= dirty_below;
      FieldMask partial;
      derez.deserialize(partial);
      state.partially_closed |= partial;
    }

    //--------------------------------------------------------------------------
    /*static*/ void RegionTreeNode::handle_logical_state_return(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      bool is_region;
      derez.deserialize(is_region);
      RegionTreeNode *target_node;
      if (is_region)
      {
        LogicalRegion handle;
        derez.deserialize(handle);
        target_node = forest->get_node(handle); 
      }
      else
      {
        LogicalPartition handle;
        derez.deserialize(handle);
        target_node = forest->get_node(handle);
      }
      std::set<RtEvent> mutator_events;
      WrapperReferenceMutator mutator(mutator_events);
      target_node->process_logical_state_return(derez, source, &mutator);
      RtUserEvent done_event;
      derez.deserialize(done_event);
      if (!mutator_events.empty())
        Runtime::trigger_event(done_event, 
            Runtime::merge_events(mutator_events));
      else
        Runtime::trigger_event(done_event);
    }

    //--------------------------------------------------------------------------
    CompositeView* RegionTreeNode::create_composite_instance(ContextID ctx_id,
                        const LegionMap<ColorPoint,FieldMask>::aligned &targets,
                                      const std::set<ColorPoint> &next_children,
                                      const FieldMask &closing_mask,
                              VersionInfo &version_info, SingleTask *target_ctx)
    //--------------------------------------------------------------------------
    {
      PhysicalState *state = get_physical_state(version_info);
      CompositeCloser closer(ctx_id, version_info, target_ctx);
      // Make the root node before traversing
      CompositeNode *root = closer.get_composite_node(this, true/*root*/);
      // Keep track of which fields are complete
      FieldMask complete_below;
      // Capture down the tree first and then capture at our local node
      // if necessary
      siphon_physical_children(closer, state, closing_mask, complete_below);
      // We are the top node, so we need to capture any field that is not 
      // complete regardless of whether it is dirty or not
      // as well as any reduction instances at this level
      FieldMask capture_dirty = closing_mask - complete_below; 
      FieldMask capture_reduc = closing_mask & state->reduction_mask;
      if (!!capture_dirty || !!capture_reduc)
        closer.capture_physical_state(root, this, state, closing_mask,
                                      capture_dirty, capture_reduc);    
      return closer.create_valid_view(state, root, closing_mask);
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::close_physical_node(CompositeCloser &closer,
                                             const FieldMask &closing_mask,
                                             FieldMask &complete_below)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime,REGION_NODE_CLOSE_COMPOSITE_NODE_CALL);
      PhysicalState *state = get_physical_state(closer.version_info);
      // Close down the tree and then determine if we have to capture locally 
      // We don't need to capture data for any fields that are complete below
      siphon_physical_children(closer, state, closing_mask, complete_below);
      // We only need to capture instances for fields which are dirty and 
      // not complete below
      FieldMask dirty_close = closing_mask & state->dirty_mask;
      // We always need to capture reductions for the fields we have
      FieldMask reduc_close = closing_mask & state->reduction_mask;
      // Quick out if there is nothing to capture
      if (!dirty_close && !reduc_close)
        return;
      // We only need to capture fields which are dirty and not already complete
      // but we must always capture any reduction fields that we have
      if (!!dirty_close && !!complete_below)
      {
        FieldMask dirty_capture = dirty_close - complete_below;
        if (!!dirty_capture || !!reduc_close)
        {
          CompositeNode *node = closer.get_composite_node(this);
          closer.capture_physical_state(node, this, state, closing_mask, 
                                        dirty_capture, reduc_close);
        }
      }
      else
      {
        CompositeNode *node = closer.get_composite_node(this);
        closer.capture_physical_state(node, this, state, closing_mask,
                                      dirty_close, reduc_close);
      }
      // Invalidate any state for which we were doing a close
      if (!!dirty_close)
      {
        invalidate_instance_views(state, dirty_close);
        state->dirty_mask -= dirty_close;
        // Update our complete mask with what we have captured
        complete_below |= dirty_close;
      }
      if (!!reduc_close)
      {
        invalidate_reduction_views(state, reduc_close);
        state->reduction_mask -= reduc_close;
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::siphon_physical_children(CompositeCloser &closer,
                                                  PhysicalState *state,
                                                  const FieldMask &closing_mask,
                                                  FieldMask &complete_mask)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime,   
                        REGION_NODE_SIPHON_COMPOSITE_CHILDREN_CALL);
#ifdef DEBUG_LEGION
      assert(state->node == this);
#endif
      // First check, if all the fields are disjoint, then we're done
      if (state->children.valid_fields * closing_mask)
        return;
      // Keep track of two sets of fields
      // 1. The set of fields for which all children are complete
      // 2. The set of fields for which any children are complete
      // Optionally in the future we can make this more precise to 
      // track when we've written to enough children to dominate
      // this node and therefore be complete
      const bool local_complete = this->is_complete();
      FieldMask all_children;
      if (local_complete)
        all_children = closing_mask;
      bool changed = false;
      std::vector<ColorPoint> to_delete;
      // Otherwise go through all of the children and 
      // see which ones we need to clean up
      for (LegionMap<ColorPoint,FieldMask>::aligned::iterator it = 
            state->children.open_children.begin(); it !=
            state->children.open_children.end(); it++)
      {
        FieldMask overlap = it->second & closing_mask;
        if (!overlap)
        {
          // Filter the all-children mask to be empty
          if (local_complete)
            all_children.clear();
          continue;
        }
        FieldMask child_complete_mask;
        std::set<ColorPoint> empty_next_children;
        bool child_complete = close_physical_child(closer, state, overlap,
                     it->first, empty_next_children, child_complete_mask);
        if (local_complete)
          all_children &= child_complete_mask;
        // Any children which are complete and have complete fields
        // automatically make us complete here too
        if (child_complete)
          complete_mask |= child_complete_mask;
        changed = true;
        it->second -= overlap;
        if (!it->second)
          to_delete.push_back(it->first);
      }
      if (local_complete && !!all_children &&
          (state->children.open_children.size() == get_num_children()))
        complete_mask |= all_children;
      // Delete any closed children
      if (!to_delete.empty())
      {
        for (std::vector<ColorPoint>::const_iterator it = to_delete.begin();
              it != to_delete.end(); it++)
        {
          state->children.open_children.erase(*it);
        }
      }
      // Rebuild the valid mask if anything changed
      if (changed)
      {
        FieldMask next_valid;
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it =
              state->children.open_children.begin(); it !=
              state->children.open_children.end(); it++)
        {
          next_valid |= it->second;
        }
        state->children.valid_fields = next_valid;
      }
    }

    //--------------------------------------------------------------------------
    bool RegionTreeNode::close_physical_child(CompositeCloser &closer,
                                              PhysicalState *state,
                                              const FieldMask &closing_mask,
                                              const ColorPoint &target_child,
                                      const std::set<ColorPoint> &next_children,
                                              FieldMask &complete_mask)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(state->node == this);
#endif
      // See if we can find the child
      LegionMap<ColorPoint,FieldMask>::aligned::iterator finder = 
        state->children.open_children.find(target_child);
      if (finder == state->children.open_children.end())
        return false;
      // Check field disjointness
      if (finder->second * closing_mask)
        return false;
      // Check for child disjointness
      if (!next_children.empty())
      {
        bool all_disjoint = true;
        for (std::set<ColorPoint>::const_iterator it = 
              next_children.begin(); it != next_children.end(); it++)
        {
          if (!are_children_disjoint(finder->first, *it))
          {
            all_disjoint = false;
            break;
          }
        }
        if (all_disjoint)
          return false;
      }
      FieldMask close_mask = finder->second & closing_mask;
      // Need to get this value before the iterator is invalidated
      RegionTreeNode *child_node = get_tree_child(finder->first);
      child_node->close_physical_node(closer, close_mask, complete_mask);
      if (!!complete_mask)
        return child_node->is_complete();
      return false;
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::open_physical_child(ContextID ctx_id,
                                             const ColorPoint &child_color,
                                             const FieldMask &open_mask,
                                             VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
      PhysicalState *state = get_physical_state(version_info);
      state->children.valid_fields |= open_mask;
      LegionMap<ColorPoint,FieldMask>::aligned::iterator finder = 
                          state->children.open_children.find(child_color);
      if (finder == state->children.open_children.end())
        state->children.open_children[child_color] = open_mask;
      else
        finder->second |= open_mask;
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::siphon_physical_children(ReductionCloser &closer,
                                                  PhysicalState *state)
    //--------------------------------------------------------------------------
    {
      if (state->children.valid_fields * closer.close_mask)
        return;
      for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it = 
            state->children.open_children.begin(); it !=
            state->children.open_children.end(); it++)
      {
        if (closer.close_mask * it->second)
          continue;
        RegionTreeNode *child = get_tree_child(it->first);
        child->close_physical_node(closer);
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::close_physical_node(ReductionCloser &closer)
    //--------------------------------------------------------------------------
    {
      PhysicalState *state = get_physical_state(closer.version_info);
      closer.issue_close_reductions(this, state);
      siphon_physical_children(closer, state);
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::find_valid_instance_views(ContextID ctx,
                                                   PhysicalState *state,
                                                   const FieldMask &valid_mask,
                                                   const FieldMask &space_mask,
                                                   VersionInfo &version_info,
                                                   bool needs_space,
                        LegionMap<LogicalView*,FieldMask>::aligned &valid_views)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, 
                        REGION_NODE_FIND_VALID_INSTANCE_VIEWS_CALL);
#ifdef DEBUG_LEGION
      assert(state->node == this);
#endif
      if (!version_info.is_upper_bound_node(this))
      {
        RegionTreeNode *parent = get_parent();
        if (parent != NULL)
        {
          FieldMask up_mask = valid_mask - state->dirty_mask;
          if (!!up_mask || needs_space)
          {
            PhysicalState *parent_state = 
              parent->get_physical_state(version_info);
            LegionMap<LogicalView*,FieldMask>::aligned local_valid;
            parent->find_valid_instance_views(ctx, parent_state, up_mask, 
                      space_mask, version_info, needs_space, local_valid);
            // Get the subview for this level
            for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator 
                  it = local_valid.begin(); it != local_valid.end(); it++)
            {
              LogicalView *local_view = it->first->get_subview(get_color());
              valid_views[local_view] = it->second;
            }
          }
        }
      }
      // Now figure out which of our valid views we can add
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
            state->valid_views.begin(); it != state->valid_views.end(); it++)
      {
        // If we need the physical instances to be at least as big as the
        // needed fields, check that first
        if (needs_space)
        {
          if (it->first->is_deferred_view())
            continue;
#ifdef DEBUG_LEGION
          assert(it->first->as_instance_view()->is_materialized_view());
#endif
          MaterializedView *current = 
            it->first->as_instance_view()->as_materialized_view();
          if (!!(space_mask - current->get_physical_mask()))
            continue;
        }
        // If we're looking for instances with space, we want the instances
        // even if they have no valid fields, otherwise if we're not looking
        // for instances with enough space, we can exit out early if they
        // don't have any valid fields
        FieldMask overlap = valid_mask & it->second;
        if (!needs_space && !overlap)
          continue;
        // Check to see if we need to merge the field views.
        LegionMap<LogicalView*,FieldMask>::aligned::iterator finder = 
          valid_views.find(it->first);
        if (finder == valid_views.end())
          valid_views[it->first] = overlap;
        else
          finder->second |= overlap;
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::find_valid_reduction_views(ContextID ctx,
                                                    PhysicalState *state,
                                                    ReductionOpID redop,
                                                    const FieldMask &valid_mask,
                                                    VersionInfo &version_info,
                                          std::set<ReductionView*> &valid_views)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, 
                        REGION_NODE_FIND_VALID_REDUCTION_VIEWS_CALL);
#ifdef DEBUG_LEGION
      assert(state->node == this);
#endif
      if (!version_info.is_upper_bound_node(this))
      {
        RegionTreeNode *parent = get_parent();
        if (parent != NULL)
        {
          // See if we can continue going up the tree
          FieldMask up_mask = valid_mask - state->dirty_mask;
          if (!!up_mask)
          {
            // Acquire the parent state in non-exclusive mode
            PhysicalState *parent_state = 
              parent->get_physical_state(version_info);
            parent->find_valid_reduction_views(ctx, parent_state, redop, 
                                    up_mask, version_info, valid_views);
          }
        }
      }
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it = 
            state->reduction_views.begin(); it != 
            state->reduction_views.end(); it++)
      {
        if (it->first->get_redop() != redop)
          continue;
        FieldMask uncovered = valid_mask - it->second;
        if (!uncovered && (valid_views.find(it->first) == valid_views.end()))
        {
          valid_views.insert(it->first);
        }
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::pull_valid_instance_views(ContextID ctx,
                                                   PhysicalState *state,
                                                   const FieldMask &mask,
                                                   bool needs_space,
                                                   VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(state->node == this);
#endif
      LegionMap<LogicalView*,FieldMask>::aligned new_valid_views;
      find_valid_instance_views(ctx, state, mask, mask, version_info,
                                needs_space, new_valid_views);
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
            new_valid_views.begin(); it != new_valid_views.end(); it++)
      {
        update_valid_views(state, it->second, false/*dirty*/, it->first);
      }
    }
    
    //--------------------------------------------------------------------------
    void RegionTreeNode::find_copy_across_instances(const TraversalInfo &info,
                                                    MaterializedView *target,
                 LegionMap<MaterializedView*,FieldMask>::aligned &src_instances,
                LegionMap<DeferredView*,FieldMask>::aligned &deferred_instances)
    //--------------------------------------------------------------------------
    {
      LegionMap<LogicalView*,FieldMask>::aligned valid_views;
      PhysicalState *state = get_physical_state(info.version_info); 
      find_valid_instance_views(info.ctx, state, info.traversal_mask,
                                info.traversal_mask, info.version_info,
                                false/*needs space*/, valid_views);
      // Now tease them apart into src and composite views and sort
      // them based on the target memory
      FieldMask copy_mask = info.traversal_mask;
      sort_copy_instances(info, target, copy_mask, valid_views,
                          src_instances, deferred_instances);
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::issue_update_copies(const TraversalInfo &info,
                                             MaterializedView *dst,
                                             FieldMask copy_mask,
             const LegionMap<LogicalView*,FieldMask>::aligned &valid_instances)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, REGION_NODE_ISSUE_UPDATE_COPIES_CALL);
#ifdef DEBUG_LEGION
      assert(!!copy_mask);
      assert(dst->logical_node == this);
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
            valid_instances.begin(); it != valid_instances.end(); it++)
      {
        if (!it->first->is_deferred_view())
          assert(it->first->logical_node == this);
      }
      // We better have space for all the fields in the request
      assert(!(copy_mask - dst->get_space_mask()));
#endif
      // Quick check to see if we are done early
      {
        LegionMap<LogicalView*,FieldMask>::aligned::const_iterator finder = 
          valid_instances.find(dst);
        if (finder != valid_instances.end())
        {
          copy_mask -= finder->second;
          if (!copy_mask)
            return;
        }
      }
      // To facilitate optimized copies in the low-level runtime, 
      // we gather all the information needed to issue gather copies 
      // from multiple instances into the data structures below, we then 
      // issue the copy when we're done and update the destination instance.
      LegionMap<MaterializedView*,FieldMask>::aligned src_instances;
      LegionMap<DeferredView*,FieldMask>::aligned deferred_instances;
      // This call updates copy_mask
      sort_copy_instances(info, dst, copy_mask, valid_instances, 
                          src_instances, deferred_instances);

      // Now we can issue the copy operation to the low-level runtime
      if (!src_instances.empty())
      {
        // Get all the preconditions for each of the different instances
        LegionMap<ApEvent,FieldMask>::aligned preconditions;
        FieldMask update_mask; 
        const AddressSpaceID local_space = context->runtime->address_space;
        for (LegionMap<MaterializedView*,FieldMask>::aligned::const_iterator 
              it = src_instances.begin(); it != src_instances.end(); it++)
        {
#ifdef DEBUG_LEGION
          assert(!!it->second);
#endif
          it->first->find_copy_preconditions(0/*redop*/, true/*reading*/,
                                             it->second, info.version_info,
                                             info.op->get_unique_op_id(),
                                             info.index, local_space, 
                                             preconditions,
                                             info.map_applied_events);
          update_mask |= it->second;
        }
        // Now do the destination
        dst->find_copy_preconditions(0/*redop*/, false/*reading*/,
                                     update_mask, info.version_info,
                                     info.op->get_unique_op_id(),
                                     info.index, local_space, preconditions,
                                     info.map_applied_events);

        LegionMap<ApEvent,FieldMask>::aligned postconditions;
        issue_grouped_copies(info, dst, preconditions, update_mask,
                 src_instances, info.version_info, postconditions);
        // Tell the destination about all of the copies that were done
        for (LegionMap<ApEvent,FieldMask>::aligned::const_iterator it = 
              postconditions.begin(); it != postconditions.end(); it++)
        {
          dst->add_copy_user(0/*redop*/, it->first, info.version_info, 
                             info.op->get_unique_op_id(), info.index,
                             it->second, false/*reading*/, local_space, 
                             info.map_applied_events);
        }
      }
      // If we still have fields that need to be updated and there
      // are composite instances then we need to issue updates copies
      // for those fields from the composite instances
      if (!deferred_instances.empty())
      {
        for (LegionMap<DeferredView*,FieldMask>::aligned::const_iterator it =
              deferred_instances.begin(); it != deferred_instances.end(); it++)
        {
          it->first->issue_deferred_copies(info, dst, it->second);
        }
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::sort_copy_instances(const TraversalInfo &info,
                                             MaterializedView *dst,
                                             FieldMask &copy_mask,
               const LegionMap<LogicalView*,FieldMask>::aligned &copy_instances,
                 LegionMap<MaterializedView*,FieldMask>::aligned &src_instances,
                LegionMap<DeferredView*,FieldMask>::aligned &deferred_instances)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, REGION_NODE_SORT_COPY_INSTANCES_CALL);
      // No need to call the mapper if there is only one valid instance
      if (copy_instances.size() == 1)
      {
        const std::pair<LogicalView*,FieldMask> &src_info = 
          *(copy_instances.begin());
        FieldMask op_mask = copy_mask & src_info.second;
        if (!!op_mask)
        {
          LogicalView *src = src_info.first;
          // No need to do anything if src and destination are the same
          // Also check for the same instance which can occur in the case
          // of deferred instances
          if (src != dst)
          {
            if (src->is_deferred_view())
            {
              DeferredView *current = src->as_deferred_view();
              deferred_instances[current] = op_mask;
            }
            else
            {
#ifdef DEBUG_LEGION
              assert(src->is_instance_view());
              assert(src->as_instance_view()->is_materialized_view());
#endif
              MaterializedView *current = 
                src->as_instance_view()->as_materialized_view();
              // If they are the same instance (which can happen with
              // composite views) then we don't need to do it
              if (current->manager->get_instance() != 
                  dst->manager->get_instance())
                src_instances[current] = op_mask;
            }
          }
        }
      }
      else if (!copy_instances.empty())
      {
        bool copy_ready = false;
        // Ask the mapper to put everything in order
        // Make the source instance set
        InstanceSet src_refs;
        std::vector<MaterializedView*> src_views;
        src_views.reserve(copy_instances.size());
        LegionMap<DeferredView*,FieldMask>::aligned available_deferred;
        // Keep track of the observed fields to see if we have 
        // multiple valid copies of the data, if we do we'll have
        // to invoke the mapper, otherwise, we know we can just do
        // them in any order.
        FieldMask observed_inst_fields;
        bool duplicate_inst_fields = false;
        for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
              copy_instances.begin(); it != copy_instances.end(); it++)
        {
          if (it->first->is_deferred_view())
          {
            DeferredView *current = it->first->as_deferred_view();
            LegionMap<DeferredView*,FieldMask>::aligned::iterator finder = 
              available_deferred.find(current);
            if (finder == available_deferred.end())
              available_deferred[current] = it->second;
            else
              finder->second |= it->second;
          }
          else
          {
#ifdef DEBUG_LEGION
            assert(it->first->is_instance_view());
            assert(it->first->as_instance_view()->is_materialized_view());
#endif
            MaterializedView *src_view = 
              it->first->as_instance_view()->as_materialized_view();
            src_refs.add_instance(
                InstanceRef(src_view->get_manager(), it->second));
            src_views.push_back(src_view);
            // If we have duplicate inst fields, record it,
            // otherwise we can just record our observed fields
            if (!duplicate_inst_fields && !(observed_inst_fields * it->second))
              duplicate_inst_fields = true;
            else
              observed_inst_fields |= it->second;
          }
        }

        if (!src_refs.empty())
        {
          std::vector<unsigned> ranking;
          // See if we have any duplicate fields in which case we
          // will need to ask the mapper to pick an order for us
          if (duplicate_inst_fields)
          {
            // Now we have to invoke the mapper to pick the order
            ranking.reserve(src_refs.size());
            InstanceRef target(dst->get_manager(), copy_mask);
            // Ask the mapper to pick the ranking
            info.op->select_sources(target, src_refs, ranking);
            // Go through all the valid instances and issue copies
            // starting first in the order that the mapper requested
            // and then through the remaining instances until all
            // the copy fields are made valid
            for (unsigned i = 0; i < ranking.size(); i++)
            {
              unsigned idx = ranking[i]; 
#ifdef DEBUG_LEGION
              assert(idx < src_views.size());
#endif
              MaterializedView *current_view = src_views[idx];
              // Mark this one NULL so we won't do it again
              src_views[idx] = NULL;
              LegionMap<LogicalView*,FieldMask>::aligned::const_iterator
                mask_finder = copy_instances.find(current_view);
#ifdef DEBUG_LEGION
              assert(mask_finder != copy_instances.end());
#endif
              FieldMask op_mask = copy_mask & mask_finder->second;
              // Check to see if there are any valid fields in the copy mask
              if (!!op_mask)
              {
                // No need to do anything if they are the same instance
                if ((dst != current_view) || (dst->manager->get_instance() != 
                                      current_view->manager->get_instance()))
                {
                  LegionMap<MaterializedView*,FieldMask>::aligned::iterator 
                    finder = src_instances.find(current_view);
                  if (finder == src_instances.end())
                    src_instances[current_view] = op_mask;
                  else
                    finder->second |= op_mask;
                }
                // Update the copy mask
                copy_mask -= op_mask;
                if (!copy_mask)
                {
                  copy_ready = true;
                  break;
                }
              }
            }
          }
          // If we still have missing fields and we didn't cover all
          // the instances, then do the rest of them
          if (!copy_ready && (ranking.size() < src_refs.size()))
          {
            for (unsigned idx = 0; idx < src_views.size(); idx++)
            {
              MaterializedView *current_view = src_views[idx];
              // See if we already considered this instance
              if (current_view == NULL)
                continue;
              LegionMap<LogicalView*,FieldMask>::aligned::const_iterator
                mask_finder = copy_instances.find(current_view);
#ifdef DEBUG_LEGION
              assert(mask_finder != copy_instances.end());
#endif
              FieldMask op_mask = copy_mask & mask_finder->second;
              // Check to see if there are any valid fields in the copy mask
              if (!!op_mask)
              {
                // No need to do anything if they are the same instance
                if ((dst != current_view) || (dst->manager->get_instance() !=
                                      current_view->manager->get_instance()))
                {
                  LegionMap<MaterializedView*,FieldMask>::aligned::iterator 
                    finder = src_instances.find(current_view);
                  if (finder == src_instances.end())
                    src_instances[current_view] = op_mask;
                  else
                    finder->second |= op_mask;
                }
                // Update the copy mask
                copy_mask -= op_mask;
                if (!copy_mask)
                {
                  copy_ready = true;
                  break;
                }
              }
            }
          }
        }
        
        // Lastly, if we are still not done, see if we have
        // any deferred instances to issue copies from
        if (!copy_ready)
        {
          for (LegionMap<DeferredView*,FieldMask>::aligned::const_iterator cit =
                available_deferred.begin(); cit != 
                available_deferred.end(); cit++)
          {
            FieldMask op_mask = copy_mask & cit->second;
            if (!!op_mask)
            {
              // No need to look for duplicates, we know this is
              // the first time this data structure can be touched
              deferred_instances[cit->first] = op_mask;
              copy_mask -= op_mask;
              if (!copy_mask)
                break;
            }
          }
        }
      }
      // Otherwise all the fields have no current data so they are
      // by defintiion up to date
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::issue_grouped_copies(const TraversalInfo &info,
                                              MaterializedView *dst,
                           LegionMap<ApEvent,FieldMask>::aligned &preconditions,
                                       const FieldMask &update_mask,
           const LegionMap<MaterializedView*,FieldMask>::aligned &src_instances,
                                       const VersionInfo &src_version_info,
                          LegionMap<ApEvent,FieldMask>::aligned &postconditions,
                                             CopyAcrossHelper *helper/*= NULL*/,
                                             RegionTreeNode *intersect/*=NULL*/)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime,REGION_NODE_ISSUE_GROUPED_COPIES_CALL);
      // Now let's build maximal sets of fields which have
      // identical event preconditions. Use a list so our
      // iterators remain valid under insertion and push back
      LegionList<EventSet>::aligned precondition_sets;
      compute_event_sets(update_mask, preconditions, precondition_sets);
      // Now that we have our precondition sets, it's time
      // to issue the distinct copies to the low-level runtime
      // Issue a copy for each of the different precondition sets
      const AddressSpaceID local_space = context->runtime->address_space;
      for (LegionList<EventSet>::aligned::iterator pit = 
            precondition_sets.begin(); pit != 
            precondition_sets.end(); pit++)
      {
        EventSet &pre_set = *pit;
        // Build the src and dst fields vectors
        std::vector<Domain::CopySrcDstField> src_fields;
        std::vector<Domain::CopySrcDstField> dst_fields;
        LegionMap<MaterializedView*,FieldMask>::aligned update_views;
        for (LegionMap<MaterializedView*,FieldMask>::aligned::const_iterator 
              it = src_instances.begin(); it != src_instances.end(); it++)
        {
          FieldMask op_mask = pre_set.set_mask & it->second;
          if (!!op_mask)
          {
            it->first->copy_from(op_mask, src_fields);
            dst->copy_to(op_mask, dst_fields, helper);
            update_views[it->first] = op_mask;
          }
        }
#ifdef DEBUG_LEGION
        assert(!src_fields.empty());
        assert(!dst_fields.empty());
        assert(src_fields.size() == dst_fields.size());
#endif
        // Now that we've got our offsets ready, we
        // can now issue the copy to the low-level runtime
        ApEvent copy_pre = Runtime::merge_events(pre_set.preconditions);
        ApEvent copy_post = issue_copy(info.op, src_fields, dst_fields, 
                                       copy_pre, intersect);
        // Save the copy post in the post conditions
        if (copy_post.exists())
        {
          // Register copy post with the source views
          // Note it is up to the caller to make sure the event
          // gets registered with the destination
          for (LegionMap<MaterializedView*,FieldMask>::aligned::const_iterator 
                it = update_views.begin(); it != update_views.end(); it++)
          {
            it->first->add_copy_user(0/*redop*/, copy_post, src_version_info,
                                     info.op->get_unique_op_id(), info.index,
                                     it->second, true/*reading*/, local_space,
                                     info.map_applied_events);
          }
          postconditions[copy_post] = pre_set.set_mask;
        }
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ void RegionTreeNode::compute_event_sets(FieldMask update_mask, 
                    const LegionMap<ApEvent,FieldMask>::aligned &preconditions,
                    LegionList<EventSet>::aligned &precondition_sets)
    //--------------------------------------------------------------------------
    {
      for (LegionMap<ApEvent,FieldMask>::aligned::const_iterator pit = 
            preconditions.begin(); pit != preconditions.end(); pit++)
      {
        bool inserted = false;
        // Also keep track of which fields have updates
        // but don't have any preconditions
        update_mask -= pit->second;
        FieldMask remaining = pit->second;
        // Insert this event into the precondition sets 
        for (LegionList<EventSet>::aligned::iterator it = 
              precondition_sets.begin(); it != precondition_sets.end(); it++)
        {
          // Easy case, check for equality
          if (remaining == it->set_mask)
          {
            it->preconditions.insert(pit->first);
            inserted = true;
            break;
          }
          FieldMask overlap = remaining & it->set_mask;
          // Easy case, they are disjoint so keep going
          if (!overlap)
            continue;
          // Moderate case, we are dominated, split into two sets
          // reusing existing set and making a new set
          if (overlap == remaining)
          {
            // Leave the existing set and make it the difference 
            it->set_mask -= overlap;
            precondition_sets.push_back(EventSet(overlap));
            EventSet &last = precondition_sets.back();
            last.preconditions = it->preconditions;
            last.preconditions.insert(pit->first);
            inserted = true;
            break;
          }
          // Moderate case, we dominate the existing set
          if (overlap == it->set_mask)
          {
            // Add ourselves to the existing set and then
            // keep going for the remaining fields
            it->preconditions.insert(pit->first);
            remaining -= overlap;
            // Can't consider ourselves added yet
            continue;
          }
          // Hard case, neither dominates, compute three
          // distinct sets of fields, keep left one in
          // place and reduce scope, add new one at the
          // end for overlap, continue iterating for right one
          it->set_mask -= overlap;
          const std::set<ApEvent> &temp_preconditions = it->preconditions;
          it = precondition_sets.insert(it, EventSet(overlap));
          it->preconditions = temp_preconditions;
          it->preconditions.insert(pit->first);
          remaining -= overlap;
          continue;
        }
        if (!inserted)
        {
          precondition_sets.push_back(EventSet(remaining));
          EventSet &last = precondition_sets.back();
          last.preconditions.insert(pit->first);
        }
      }
      // For any fields which need copies but don't have
      // any preconditions, but them in their own set.
      // Put it on the front because it is the copy with
      // no preconditions so it can start right away!
      if (!!update_mask)
        precondition_sets.push_front(EventSet(update_mask));
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::issue_update_reductions(LogicalView *target,
                                                 const FieldMask &mask,
                                                const VersionInfo &version_info,
           const LegionMap<ReductionView*,FieldMask>::aligned &valid_reductions,
                                                 Operation *op, unsigned index,
                                                 std::set<RtEvent> &map_applied)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, 
                        REGION_NODE_ISSUE_UPDATE_REDUCTIONS_CALL);
      // We should never get a deferred view in here, this function
      // should only be called when the targets are guaranteed to all
      // be actual physical instances
#ifdef DEBUG_LEGION
      assert(target->is_instance_view());
#endif
      InstanceView *inst_target = target->as_instance_view();
      // Go through all of our reduction instances and issue reductions
      // to the target instances
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it = 
            valid_reductions.begin(); it != valid_reductions.end(); it++)
      {
        // Doesn't need to reduce to itself
        if (inst_target == (it->first))
          continue;
        FieldMask copy_mask = mask & it->second;
        if (!!copy_mask)
        {
#ifdef DEBUG_LEGION
          // all fields in the reduction instances should be used
          assert(!(it->second - copy_mask));
#endif
          // Then we have a reduction to perform
          it->first->perform_reduction(inst_target, copy_mask, 
                                       version_info, op, index, map_applied);
        }
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::invalidate_instance_views(PhysicalState *state,
                                                  const FieldMask &invalid_mask)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(state->node == this);
#endif
      std::vector<LogicalView*> to_delete;
      for (LegionMap<LogicalView*,FieldMask>::aligned::iterator it = 
            state->valid_views.begin(); it != state->valid_views.end(); it++)
      {
        it->second -= invalid_mask;
        if (!it->second)
          to_delete.push_back(it->first);
      }
      for (std::vector<LogicalView*>::const_iterator it = to_delete.begin();
            it != to_delete.end(); it++)
      {
        state->valid_views.erase(*it);
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::filter_valid_views(PhysicalState *state, 
                                            LogicalView *to_filter)
    //--------------------------------------------------------------------------
    {
      LegionMap<LogicalView*,FieldMask>::aligned::iterator finder = 
        state->valid_views.find(to_filter);
      if (finder != state->valid_views.end())
        state->valid_views.erase(finder);
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::invalidate_reduction_views(PhysicalState *state,
                                                  const FieldMask &invalid_mask)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(state->node == this);
#endif
      std::vector<ReductionView*> to_delete;
      for (LegionMap<ReductionView*,FieldMask>::aligned::iterator it = 
            state->reduction_views.begin(); it != 
            state->reduction_views.end(); it++)
      {
        it->second -= invalid_mask;
        if (!it->second)
          to_delete.push_back(it->first);
      }
      for (std::vector<ReductionView*>::const_iterator it = to_delete.begin();
            it != to_delete.end(); it++)
      {
        state->reduction_views.erase(*it);
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::issue_restricted_copies(const TraversalInfo &info,
        const InstanceSet &restricted_instances,
        const std::vector<MaterializedView*> &restricted_views,
        const LegionMap<LogicalView*,FieldMask>::aligned &copy_out_views)
    //--------------------------------------------------------------------------
    {
      FieldMask src_fields;
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
            copy_out_views.begin(); it != copy_out_views.end(); it++)
        src_fields |= it->second;
      // For each one of our restricted views, issue an update copy
      for (unsigned idx = 0; idx < restricted_views.size(); idx++)
      {
        FieldMask overlap = src_fields & 
                              restricted_instances[idx].get_valid_fields();
        if (!overlap)
          continue;
        src_fields -= overlap;
        // If it's already the same there is nothing to do
        LegionMap<LogicalView*,FieldMask>::aligned::const_iterator finder =
          copy_out_views.find(restricted_views[idx]);
        if (finder != copy_out_views.end())
        {
          overlap -= finder->second;
          if (!overlap)
          {
            if (!src_fields)
              break;
            continue;
          }
        }
        // Issue the update copy
        issue_update_copies(info, restricted_views[idx], 
                            overlap, copy_out_views);
        if (!src_fields)
          break;
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::issue_restricted_reductions(const TraversalInfo &info,
        const InstanceSet &restricted_instances,
        const std::vector<MaterializedView*> &restricted_views,
        const LegionMap<ReductionView*,FieldMask>::aligned &reduce_out_views)
    //--------------------------------------------------------------------------
    {
      FieldMask reduction_fields;
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it = 
            reduce_out_views.begin(); it != reduce_out_views.end(); it++)
        reduction_fields |= it->second;
      // Issue updates for any of reductions to the targets
      for (unsigned idx = 0; idx < restricted_views.size(); idx++)
      {
        FieldMask overlap = reduction_fields &
                              restricted_instances[idx].get_valid_fields();
        if (!overlap)
          continue;
        issue_update_reductions(restricted_views[idx], overlap, 
                                info.version_info, reduce_out_views,
                                info.op, info.index, info.map_applied_events);
        reduction_fields -= overlap;
        if (!reduction_fields)
          break;
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::update_valid_views(PhysicalState *state, 
                                            const FieldMask &valid_mask,
                                            bool dirty, LogicalView *new_view)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(state->node == this);
      if (!new_view->is_deferred_view())
      {
        assert(new_view->is_instance_view());
        assert(new_view->as_instance_view()->is_materialized_view());
        assert(!(valid_mask - new_view->as_instance_view()->
                as_materialized_view()->manager->layout->allocated_fields));
        assert(new_view->logical_node == this);
      }
#endif
      if (dirty)
      {
        invalidate_instance_views(state, valid_mask); 
        state->dirty_mask |= valid_mask;
      }
      LegionMap<LogicalView*,FieldMask>::aligned::iterator finder = 
        state->valid_views.find(new_view);
      if (finder == state->valid_views.end())
      {
        // New valid view, update everything accordingly
        state->valid_views[new_view] = valid_mask;
      }
      else
      {
        // It already existed update the valid mask
        finder->second |= valid_mask;
      }
    } 

    //--------------------------------------------------------------------------
    void RegionTreeNode::update_valid_views(PhysicalState *state, 
                                            const FieldMask &dirty_mask,
                                const std::vector<LogicalView*> &new_views,
                                const InstanceSet &corresponding_references)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(state->node == this);
      assert(new_views.size() == corresponding_references.size());
#endif
      if (!!dirty_mask)
      {
        invalidate_instance_views(state, dirty_mask); 
        state->dirty_mask |= dirty_mask;
      }
      for (unsigned idx = 0; idx < new_views.size(); idx++)
      {
        LogicalView *new_view = new_views[idx];
        const FieldMask &valid_mask = 
          corresponding_references[idx].get_valid_fields();
        LegionMap<LogicalView*,FieldMask>::aligned::iterator finder = 
          state->valid_views.find(new_view);
        // If it is a new view, we can just add the fields
        if (finder == state->valid_views.end())
          state->valid_views[new_view] = valid_mask;
        else // it already exists, so update the mask
          finder->second |= valid_mask;
#ifdef DEBUG_LEGION
        if (!new_view->is_deferred_view())
        {
          assert(new_view->as_instance_view()->is_materialized_view());
          MaterializedView *mat_view = 
            new_view->as_instance_view()->as_materialized_view();
          finder = state->valid_views.find(new_view);
          assert(!(finder->second - 
                          mat_view->manager->layout->allocated_fields));
          assert(new_view->logical_node == this);
        }
#endif
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::update_valid_views(PhysicalState *state,
                                            const FieldMask &dirty_mask,
                               const std::vector<InstanceView*> &new_views,
                               const InstanceSet &corresponding_references)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(state->node == this);
      assert(new_views.size() == corresponding_references.size());
#endif
      if (!!dirty_mask)
      {
        invalidate_instance_views(state, dirty_mask); 
        state->dirty_mask |= dirty_mask;
      }
      for (unsigned idx = 0; idx < new_views.size(); idx++)
      {
        InstanceView *new_view = new_views[idx];
        const FieldMask &valid_mask = 
          corresponding_references[idx].get_valid_fields();
        LegionMap<LogicalView*,FieldMask>::aligned::iterator finder = 
          state->valid_views.find(new_view);
        // If it is a new view, we can just add the fields
        if (finder == state->valid_views.end())
          state->valid_views[new_view] = valid_mask;
        else // it already exists, so update the mask
          finder->second |= valid_mask;
#ifdef DEBUG_LEGION
        assert(new_view->is_materialized_view());
        MaterializedView *mat_view = 
          new_view->as_instance_view()->as_materialized_view();
        finder = state->valid_views.find(new_view);
        assert(!(finder->second - 
                        mat_view->manager->layout->allocated_fields));
        assert(new_view->logical_node == this);
#endif
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::update_valid_views(PhysicalState *state, 
                                            const FieldMask &dirty_mask,
                                const std::vector<MaterializedView*> &new_views,
                                const InstanceSet &corresponding_references)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(state->node == this);
      assert(new_views.size() == corresponding_references.size());
#endif
      if (!!dirty_mask)
      {
        invalidate_instance_views(state, dirty_mask); 
        state->dirty_mask |= dirty_mask;
      }
      for (unsigned idx = 0; idx < new_views.size(); idx++)
      {
        MaterializedView *new_view = new_views[idx];
        const FieldMask &valid_mask = 
          corresponding_references[idx].get_valid_fields();
        LegionMap<LogicalView*,FieldMask>::aligned::iterator finder = 
          state->valid_views.find(new_view);
        // If it is a new view, we can just add the fields
        if (finder == state->valid_views.end())
          state->valid_views[new_view] = valid_mask;
        else // it already exists, so update the mask
          finder->second |= valid_mask;
#ifdef DEBUG_LEGION
        finder = state->valid_views.find(new_view);
        assert(!(finder->second - new_view->manager->layout->allocated_fields));
        assert(new_view->logical_node == this);
#endif
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::update_reduction_views(PhysicalState *state,
                                                const FieldMask &valid_mask,
                                                ReductionView *new_view) 
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(state->node == this);
#endif
      LegionMap<ReductionView*,FieldMask>::aligned::iterator finder = 
        state->reduction_views.find(new_view);
      if (finder == state->reduction_views.end())
      {
        state->reduction_views[new_view] = valid_mask;
      }
      else
      {
        finder->second |= valid_mask;
      }
      state->reduction_mask |= valid_mask;
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::flush_reductions(const FieldMask &valid_mask,
                                          ReductionOpID redop,
                                          const TraversalInfo &info)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, REGION_NODE_FLUSH_REDUCTIONS_CALL);
      // Go through the list of reduction views and see if there are
      // any that don't mesh with the current user and therefore need
      // to be flushed.
      FieldMask flush_mask;
      LegionMap<LogicalView*,FieldMask>::aligned valid_views;
      LegionMap<ReductionView*,FieldMask>::aligned reduction_views;
      PhysicalState *state = get_physical_state(info.version_info); 
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it =
            state->reduction_views.begin(); it != 
            state->reduction_views.end(); it++)
      {
        // Skip reductions that have the same reduction op ID
        if (it->first->get_redop() == redop)
          continue;
        FieldMask overlap = valid_mask & it->second;
        if (!overlap)
          continue;
        flush_mask |= overlap; 
        reduction_views.insert(*it);
      }
      // Now get any physical instances to flush to
      if (!!flush_mask)
      {
        find_valid_instance_views(info.ctx, state, flush_mask, flush_mask, 
                    info.version_info, false/*needs space*/, valid_views);
      }
      if (!!flush_mask)
      {
        FieldMask update_mask;
        // Iterate over all the valid instances and issue any reductions
        // to the target that need to be done
        for (LegionMap<LogicalView*,FieldMask>::aligned::iterator it = 
              valid_views.begin(); it != valid_views.end(); it++)
        {
          if (it->first->is_deferred_view())
            continue;
          FieldMask overlap = flush_mask & it->second; 
          issue_update_reductions(it->first, overlap, info.version_info,
              reduction_views, info.op, info.index, info.map_applied_events);
          // Save the overlap fields
          it->second = overlap;
          update_mask |= overlap;
        }
        // We should have issued reduction operations to at least
        // one place for every single reduction field, if we didn't
        // then issue a warning
        if (!!(flush_mask - update_mask))
          log_run.warning("WARNING: No valid instances found for reduction "
                          "fields. This is a result of performing reductions "
                          "to an uninitialized field. Undefined behavior "
                          "may occur.");
        // Now update our physical state
        // Update the valid views.  Don't mark them dirty since we
        // don't want to accidentally invalidate some of our other
        // instances which get updated later in the loop.  Note this
        // is safe since we're updating all the instances for each
        // reduction field.
        for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it =
              valid_views.begin(); it != valid_views.end(); it++)
        {
          update_valid_views(state, it->second, false/*dirty*/, it->first);
        }
        // Update the dirty mask since we didn't do it when updating
        // the valid instance views do it now
        state->dirty_mask |= flush_mask;
        // Then invalidate all the reduction views that we flushed
        invalidate_reduction_views(state, flush_mask);
      }
    } 

    //--------------------------------------------------------------------------
    void RegionTreeNode::find_complete_fields(const FieldMask &scope_fields,
                       const LegionMap<ColorPoint,FieldMask>::aligned &children,
                       FieldMask &complete_fields)
    //--------------------------------------------------------------------------
    {
      for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        FieldMask test_fields = it->second - complete_fields;
        // No fields to test so we are done
        if (!test_fields)
          continue;
        RegionTreeNode *child = get_tree_child(it->first);
        if (child->is_complete())
        {
          complete_fields |= test_fields;
          // If we proved that they are all complete, we are done
          if (scope_fields == complete_fields)
            break;
        }
      }
    }

    //--------------------------------------------------------------------------
    InstanceView* RegionTreeNode::convert_reference(const InstanceRef &ref,
                                                    SingleTask *context)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!ref.is_composite_ref());
#endif
      PhysicalManager *manager = ref.get_manager();
#ifdef DEBUG_LEGION
      // Small sanity check to make sure they are in the same tree
      assert(get_tree_id() == manager->region_node->get_tree_id());
      assert(manager->region_node->context == this->context);
#endif
      // Check to see if we've already registered it
      InstanceView *result = find_instance_view(manager, context);
      if (result != NULL)
        return result;
      // We have to make it if it doesn't exist yet
      // If we're at the root, get the view we need for this context
      if (manager->region_node == this)
        result = context->create_instance_top_view(manager, 
                                    context->runtime->address_space);
      // If we didn't find it immediately, switch over to the explicit
      // versions that don't have so many virtual function calls
      else if (is_region())
        result = as_region_node()->convert_reference_region(manager, context);
      else
        result = 
          as_partition_node()->convert_reference_partition(manager, context); 
      return result;
    }

    //--------------------------------------------------------------------------
    CompositeView* RegionTreeNode::convert_reference(
                                                   const InstanceRef &ref) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(ref.is_composite_ref());
#endif
      CompositeView *view = ref.get_composite_view();
#ifdef DEBUG_LEGION
      assert(get_tree_id() == view->logical_node->get_tree_id());
#endif
      // If we're already at the root then we are done
      if (view->logical_node == this)
        return view;
      if (is_region())
        return as_region_node()->convert_composite_view_region(view);
      else
        return as_partition_node()->convert_composite_view_partition(view);
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::convert_target_views(const InstanceSet &targets,
                  SingleTask *context, std::vector<InstanceView*> &target_views)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(targets.size() == target_views.size());
#endif
      if (targets.size() == 1)
      {
        // If we just have one there is a fast path 
        target_views[0] = convert_reference(targets[0], context);
      }
      else
      {
        // Deduplicate the instances and then go up for any that we still need
        std::map<PhysicalManager*,unsigned/*idx*/> deduplicated;
        std::vector<PhysicalManager*> to_convert;
        for (unsigned idx = 0; idx < targets.size(); idx++)
        {
          PhysicalManager *manager = targets[idx].get_manager();
          target_views[idx] = find_instance_view(manager, context);
          // Quick check to see if we are done early
          if (target_views[idx] != NULL)
            continue;
          // If we didn't find it we will have to go looking for it 
          // See if we've got this one already or not
          if (deduplicated.find(manager) == deduplicated.end())
          {
            unsigned next = deduplicated.size();
            deduplicated[manager] = next;
            to_convert.push_back(manager);
          }
        }
        if (to_convert.size() == 1)
        {
          // Easy case, there is only one deduplicated instance
          InstanceView *result = is_region() ? 
            as_region_node()->convert_reference_region(to_convert[0], context) :
            as_partition_node()->convert_reference_partition(to_convert[0], 
                                                             context);
          for (unsigned idx = 0; idx < target_views.size(); idx++)
          {
            if (target_views[idx] != NULL) // skip if we already did it
              continue;
            target_views[idx] = result;
          }
        }
        else
        {
          // Go up the tree for all the unhandled ones
          std::vector<bool> up_mask(to_convert.size(), true);
          std::vector<InstanceView*> results(to_convert.size(), NULL);
          if (is_region())
            as_region_node()->convert_references_region(to_convert, 
                                        up_mask, context, results);
          else
            as_partition_node()->convert_references_partition(to_convert,
                                        up_mask, context, results);
          for (unsigned idx = 0; idx < target_views.size(); idx++)
          {
            if (target_views[idx] != NULL) // skip it if we already did it
              continue;
            PhysicalManager *manager = targets[idx].get_manager();
#ifdef DEBUG_LEGION
            assert(deduplicated.find(manager) != deduplicated.end());
#endif
            target_views[idx] = results[deduplicated[manager]];
#ifdef DEBUG_LEGION
            assert(target_views[idx] != NULL);
#endif
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::convert_target_views(const InstanceSet &targets,
              SingleTask *context, std::vector<MaterializedView*> &target_views)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(targets.size() == target_views.size());
#endif
      std::vector<InstanceView*> instance_views(target_views.size(), NULL);
      // Call the instance view version
      convert_target_views(targets, context, instance_views);
      // Then do our conversion
      for (unsigned idx = 0; idx < targets.size(); idx++)
      {
        InstanceView *inst_view = instance_views[idx];
#ifdef DEBUG_LEGION
        assert(inst_view->is_materialized_view());
#endif
        target_views[idx] = inst_view->as_materialized_view();
      }
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::initialize_current_state(ContextID ctx)
    //--------------------------------------------------------------------------
    {
      if (!current_states.has_entry(ctx))
        return;
      CurrentState &state = get_current_state(ctx);
      state.check_init();
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::invalidate_current_state(ContextID ctx,
                                                  bool logical_users_only)
    //--------------------------------------------------------------------------
    {
      if (!current_states.has_entry(ctx))
        return;
      CurrentState &state = get_current_state(ctx);
      if (logical_users_only)
        state.clear_logical_users();
      else
        state.reset(); 
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::invalidate_deleted_state(ContextID ctx,
                                                  const FieldMask &deleted_mask)
    //--------------------------------------------------------------------------
    {
      if (!current_states.has_entry(ctx))
        return;
      CurrentState &state = get_current_state(ctx);
      state.clear_deleted_state(deleted_mask);
    }
    
    //--------------------------------------------------------------------------
    bool RegionTreeNode::register_instance_view(PhysicalManager *manager,
                                       UniqueID context_uid, InstanceView *view)
    //--------------------------------------------------------------------------
    {
      std::pair<PhysicalManager*,UniqueID> key(manager, context_uid);
      AutoLock n_lock(node_lock);
      LegionMap<std::pair<PhysicalManager*,UniqueID>,InstanceView*>::
        tracked::const_iterator finder = instance_views.find(key);
      if (finder != instance_views.end())
      {
        // Duplicate registrations are alright
        if (finder->second == view)
          return true;
        return false;
      }
      instance_views[key] = view;
      return true;
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::unregister_instance_view(PhysicalManager *manager,
                                                  UniqueID context_uid)
    //--------------------------------------------------------------------------
    {
      std::pair<PhysicalManager*,UniqueID> key(manager, context_uid);
      AutoLock n_lock(node_lock);
      LegionMap<std::pair<PhysicalManager*,UniqueID>,InstanceView*>::
        tracked::iterator finder = instance_views.find(key);
      if (finder != instance_views.end())
        instance_views.erase(finder);
    }

    //--------------------------------------------------------------------------
    InstanceView* RegionTreeNode::find_instance_view(PhysicalManager *manager,
                                                     SingleTask *context) 
    //--------------------------------------------------------------------------
    {
      std::pair<PhysicalManager*,UniqueID> key(manager, 
                                                  context->get_context_uid());
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      LegionMap<std::pair<PhysicalManager*,UniqueID>,InstanceView*>::
        tracked::const_iterator finder = instance_views.find(key);
      if (finder != instance_views.end())
        return finder->second;
      return NULL;
    }

    //--------------------------------------------------------------------------
    VersionState* RegionTreeNode::find_remote_version_state(VersionID vid, 
       DistributedID did, AddressSpaceID owner_space, ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
      // Use the node lock to ensure that we don't
      // replicated version states on a node
      VersionState *result = NULL;
      Runtime *runtime = context->runtime;
      {
        AutoLock n_lock(node_lock);
        if (runtime->has_distributed_collectable(did))
        {
          DistributedCollectable *dc = 
            runtime->find_distributed_collectable(did);
#ifdef DEBUG_LEGION
          result = dynamic_cast<VersionState*>(dc);
          assert(result != NULL);
#else
          result = static_cast<VersionState*>(dc);
#endif
        }
        else // Otherwise make it
        {
          AddressSpaceID local_space = runtime->address_space;
#ifdef DEBUG_LEGION
          assert(owner_space != local_space);
#endif
          result = legion_new<VersionState>(vid, context->runtime, 
              did, owner_space, local_space, this, false/*register now*/);
          result->register_with_runtime(mutator);
        }
      }
      return result;
    }

    //--------------------------------------------------------------------------
    VersionState* RegionTreeNode::create_new_version_state(VersionID vid)
    //--------------------------------------------------------------------------
    {
      DistributedID new_did = 
        context->runtime->get_available_distributed_id(false);
      AddressSpace local_space = context->runtime->address_space;
      return legion_new<VersionState>(vid, context->runtime, 
          new_did, local_space, local_space, this, true/*register now*/);
    }

    //--------------------------------------------------------------------------
    bool RegionTreeNode::register_physical_manager(PhysicalManager *manager)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      if (physical_managers.find(manager->did) != physical_managers.end())
        return false;
      physical_managers[manager->did] = manager;
      return true;
    }

    //--------------------------------------------------------------------------
    void RegionTreeNode::unregister_physical_manager(PhysicalManager *manager)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      physical_managers.erase(manager->did);
    }

    //--------------------------------------------------------------------------
    PhysicalManager* RegionTreeNode::find_manager(DistributedID did)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      LegionMap<DistributedID,PhysicalManager*,
                PHYSICAL_MANAGER_ALLOC>::tracked::const_iterator finder = 
                  physical_managers.find(did);
#ifdef DEBUG_LEGION
      assert(finder != physical_managers.end());
#endif
      return finder->second;
    }

    //--------------------------------------------------------------------------
    template<AllocationType ALLOC, bool RECORD, bool HAS_SKIP, bool TRACK_DOM>
    /*static*/ FieldMask RegionTreeNode::perform_dependence_checks(
      const LogicalUser &user, 
      typename LegionList<LogicalUser, ALLOC>::track_aligned &prev_users,
      const FieldMask &check_mask, const FieldMask &open_below,
      bool validates_regions, Operation *to_skip /*= NULL*/, 
      GenerationID skip_gen /* = 0*/)
    //--------------------------------------------------------------------------
    {
      FieldMask dominator_mask = check_mask;
      // It's not actually sound to assume we dominate something
      // if we don't observe any users of those fields.  Therefore
      // also keep track of the fields that we observe.  We'll use this
      // at the end when computing the final dominator mask.
      FieldMask observed_mask; 
      FieldMask user_check_mask = user.field_mask & check_mask;
      const bool tracing = user.op->is_tracing();
      for (typename LegionList<LogicalUser, ALLOC>::track_aligned::iterator 
            it = prev_users.begin(); it != prev_users.end(); /*nothing*/)
      {
        if (HAS_SKIP && (to_skip == it->op) && (skip_gen == it->gen))
        {
          it++;
          continue;
        }
        FieldMask overlap = user_check_mask & it->field_mask;
        if (!!overlap)
        {
          if (TRACK_DOM)
            observed_mask |= overlap;
          DependenceType dtype = check_dependence_type(it->usage, user.usage);
          bool validate = validates_regions;
          switch (dtype)
          {
            case NO_DEPENDENCE:
              {
                // No dependence so remove bits from the dominator mask
                dominator_mask -= it->field_mask;
                break;
              }
            case ANTI_DEPENDENCE:
            case ATOMIC_DEPENDENCE:
            case SIMULTANEOUS_DEPENDENCE:
            case PROMOTED_DEPENDENCE:
              {
                // Mark that these kinds of dependences are not allowed
                // to validate region inputs
                validate = false;
                // No break so we register dependences just like
                // a true dependence
              }
            case TRUE_DEPENDENCE:
              {
#ifdef LEGION_SPY
                if (dtype != PROMOTED_DEPENDENCE)
                  LegionSpy::log_mapping_dependence(
                      user.op->get_parent()->get_unique_id(),
                      it->uid, it->idx, user.uid, user.idx, dtype);
#endif
                if (RECORD)
                  user.op->record_logical_dependence(*it);
                // Do this after the logging since we might 
                // update the iterator.
                // If we can validate a region record which of our
                // predecessors regions we are validating, otherwise
                // just register a normal dependence
                if (user.op->register_region_dependence(user.idx, it->op, 
                                                        it->gen, it->idx,
                                                        dtype, validate,
                                                        overlap))
                {
#ifndef LEGION_SPY
                  // Now we can prune it from the list and continue
                  it = prev_users.erase(it);
#else
                  it++;
#endif
                  continue;
                }
                else
                {
                  // hasn't commited, reset timeout and continue
                  it->timeout = LogicalUser::TIMEOUT;
                  it++;
                  continue;
                }
                break;
              }
            default:
              assert(false); // should never get here
          }
        }
        // If we didn't register any kind of dependence, check
        // to see if the timeout has expired.  Note that it is
        // unsound to do this if we are tracing so don't perform
        // the check in that case.
        if (!tracing)
        {
          if (it->timeout <= 0)
          {
            // Timeout has expired.  Check whether the operation
            // has committed. If it has prune it from the list.
            // Otherwise reset its timeout and continue.
            if (it->op->is_operation_committed(it->gen))
            {
#ifndef LEGION_SPY
              it = prev_users.erase(it);
#else
              // Can't prune things early for these cases
              it->timeout = LogicalUser::TIMEOUT;
              it++;
#endif
            }
            else
            {
              // Operation hasn't committed, reset timeout
              it->timeout = LogicalUser::TIMEOUT;
              it++;
            }
          }
          else
          {
            // Timeout hasn't expired, decrement it and continue
            it->timeout--;
            it++;
          }
        }
        else
          it++; // Tracing so no timeouts
      }
      // The result of this computation is the dominator mask.
      // It's only sound to say that we dominate fields that
      // we actually observed users for so intersect the dominator 
      // mask with the observed mask
      if (TRACK_DOM)
      {
        // For writes, there is a special case here we actually
        // want to record that we are dominating fields which 
        // are not actually open below even if we didn't see
        // any users on the way down
        if (IS_WRITE(user.usage))
        {
          FieldMask unobserved = check_mask - observed_mask;
          if (!!unobserved)
          {
            if (!open_below)
              observed_mask |= unobserved;
            else
              observed_mask |= (unobserved - open_below);
          }
        }
        return (dominator_mask & observed_mask);
      }
      else
        return dominator_mask;
    }

    // This function is a little out of place to make sure we get the 
    // templates instantiated properly
    //--------------------------------------------------------------------------
    void LogicalCloser::register_dependences(const LogicalUser &current,
                                             const FieldMask &open_below,
           LegionMap<TraceCloseOp*,LogicalUser>::aligned &closes,
           LegionMap<ColorPoint,ClosingInfo>::aligned &children,
           LegionList<LogicalUser,LOGICAL_REC_ALLOC >::track_aligned &abv_users,
           LegionList<LogicalUser,CURR_LOGICAL_ALLOC>::track_aligned &cur_users,
           LegionList<LogicalUser,PREV_LOGICAL_ALLOC>::track_aligned &pre_users)
    //--------------------------------------------------------------------------
    {
      // Start dependence analysis for all our closes
      for (LegionMap<TraceCloseOp*,LogicalUser>::aligned::iterator op_it = 
            closes.begin(); op_it != closes.end(); op_it++)
      {
        // Mark that we are starting our dependence analysis
        op_it->first->begin_dependence_analysis();
        // First tell the operation to register dependences on any children
        // Register dependences on any interfering children
        // We know that only field non-interference is interesting here
        // because close operations have READ_WRITE EXCLUSIVE
        const FieldMask close_op_mask = op_it->second.field_mask;
        // Get the set of children being closed
        const LegionMap<ColorPoint,FieldMask>::aligned &colors = 
                                        op_it->first->get_target_children();
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator 
              cit = colors.begin(); cit != colors.end(); cit++)
        {
          LegionMap<ColorPoint,ClosingInfo>::aligned::iterator finder = 
                                                  children.find(cit->first);
#ifdef DEBUG_LEGION
          assert(finder != children.end());
#endif
          LegionList<LogicalUser,CLOSE_LOGICAL_ALLOC>::track_aligned 
            &child_users = finder->second.child_users;
          // A tricky case here.  We know the current operation is
          // going to register dependences on this close operation,
          // so we can't have the close operation register depencnes
          // on any other users from the same op as the current one
          // we are doing the analysis for (e.g. other region reqs)
          RegionTreeNode::perform_dependence_checks<CLOSE_LOGICAL_ALLOC,
            false/*record*/, true/*has skip*/, false/*track dom*/>(
                                        op_it->second, child_users,
                                        close_op_mask, open_below,
                                        false/*validates*/,
                                        current.op, current.gen);
          // Remove any overlapping fields, we know they won't
          // be used in any other close operations
          finder->second.child_fields -= close_op_mask;
          // If we've checked against all our fields then we are done
          if (!finder->second.child_fields)
          {
            // We own mapping references on each one of the users so 
            // we need to remove them once we are done
            for (LegionList<LogicalUser,CLOSE_LOGICAL_ALLOC>::
                  track_aligned::const_iterator it = child_users.begin(); 
                  it != child_users.end(); it++)
            {
              it->op->remove_mapping_reference(it->gen);
            }
            children.erase(finder);
          }
        }
        // Next do checks against any operations above in the tree which
        // the operation already recorded dependences. No need for skip
        // here because we know the operation didn't register any 
        // dependences against itself.
        if (!abv_users.empty())
          RegionTreeNode::perform_dependence_checks<LOGICAL_REC_ALLOC,
              false/*record*/, false/*has skip*/, false/*track dom*/>(
                                         op_it->second, abv_users,
                                         close_op_mask, open_below,
                                         false/*validates*/,
                                         current.op, current.gen);
        // Finally register any dependences on users at this level
        // See the note above the for the tricky case
        FieldMask dominator_mask; 
        if (!cur_users.empty())
          dominator_mask = 
            RegionTreeNode::perform_dependence_checks<CURR_LOGICAL_ALLOC,
              false/*record*/, true/*has skip*/, true/*track dom*/>(
                                        op_it->second, cur_users,
                                        close_op_mask, open_below,
                                        false/*validates*/,
                                        current.op, current.gen);
        FieldMask non_dominated_mask = close_op_mask - dominator_mask;
        if (!!non_dominated_mask && !pre_users.empty())
          RegionTreeNode::perform_dependence_checks<PREV_LOGICAL_ALLOC,
            false/*record*/, true/*has skip*/, false/*track dom*/>(
                                 op_it->second, pre_users, 
                                 non_dominated_mask, open_below,
                                 false/*validates*/,
                                 current.op, current.gen);
        // Before we kick off this operation, add a mapping
        // reference to it since we know we are going to put it
        // in the state of the logical region tree
        op_it->first->add_mapping_reference(op_it->first->get_generation());
        // Mark that we are done, this puts the close op in the pipeline!
        // This is why we cache the LogicalUser before kicking off the op
        op_it->first->end_dependence_analysis();
      }
    }

    //--------------------------------------------------------------------------
    template<AllocationType ALLOC>
    /*static*/ void RegionTreeNode::perform_closing_checks(
        LogicalCloser &closer, 
        typename LegionList<LogicalUser, ALLOC>::track_aligned &users, 
        const FieldMask &check_mask)
    //--------------------------------------------------------------------------
    {
      // Since we are performing a close operation on the region
      // tree data structure, we know that we need to register
      // mapping dependences on all of the operations in the 
      // current epoch since close operations must be serialized
      // with respect to mappings.  Finally we have to upgrade the
      // privilege to read-write to ensure that anyone that comes
      // later also records mapping dependences on the users.
      const FieldMask user_check_mask = closer.user.field_mask & check_mask; 
      for (typename LegionList<LogicalUser, ALLOC>::track_aligned::iterator 
            it = users.begin(); it != users.end(); /*nothing*/)
      {
        FieldMask overlap = user_check_mask & it->field_mask;
        if (!overlap)
        {
          it++;
          continue;
        }
        // Skip any users of the same op, we know they won't be dependences
        if ((it->op == closer.user.op) && (it->gen == closer.user.gen))
        {
          // Report the interfering close operation
          it->op->report_interfering_close_requirement(it->idx);
          it->field_mask -= overlap; 
          if (!it->field_mask)
          {
            it->op->remove_mapping_reference(it->gen);
            it = users.erase(it);
          }
          else
            it++;
          continue;
        }

        if (closer.capture_users)
        {
          // Record that we closed this user
          // Update the field mask and the privilege
          closer.closed_users.push_back(*it);
          LogicalUser &closed_user = closer.closed_users.back();
          closed_user.field_mask = overlap;
          closed_user.usage.privilege = 
            (PrivilegeMode)((int)closed_user.usage.privilege | PROMOTED);
          // Remove the closed set of fields from this user
          it->field_mask -= overlap;
          // If it's empty, remove it from the list and let
          // the mapping reference go up the tree with it
          // Otherwise add a new mapping reference
          if (!it->field_mask)
            it = users.erase(it);
          else
          {
            it->op->add_mapping_reference(it->gen);
            it++;
          }
        }
        else
        {
          // If we're not capturing the users, then we actually
          // have to do the dependence analysis with respect to 
          // the closing user
          DependenceType dtype = check_dependence_type(it->usage, 
                                                     closer.user.usage);
#ifdef LEGION_SPY
          if ((dtype != NO_DEPENDENCE) && (dtype != PROMOTED_DEPENDENCE))
            LegionSpy::log_mapping_dependence(
                closer.user.op->get_parent()->get_unique_id(),
                it->uid, it->idx, closer.user.uid, closer.user.idx, dtype);
#endif
          // Register the dependence 
          if (closer.user.op->register_region_dependence(closer.user.idx, 
                                                         it->op, it->gen, 
                                                         it->idx, dtype,
                                                         closer.validates,
                                                         overlap))
          {
#ifndef LEGION_SPY
            it = users.erase(it);
            continue;
#endif
          }
          else
          {
            // it hasn't committed, reset timeout
            it->timeout = LogicalUser::TIMEOUT;
          }
          // Remove the closed set of fields from this user
          it->field_mask -= overlap;
          // Otherwise, if we can remote it, then remove it's
          // mapping reference from the logical tree.
          if (!it->field_mask)
          {
            it->op->remove_mapping_reference(it->gen);
            it = users.erase(it);
          }
          else
            it++;
        }
      }
    }

    /////////////////////////////////////////////////////////////
    // Region Node 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    RegionNode::RegionNode(LogicalRegion r, PartitionNode *par,
                           IndexSpaceNode *row_src, FieldSpaceNode *col_src,
                           RegionTreeForest *ctx)
      : RegionTreeNode(ctx, col_src), handle(r), 
        parent(par), row_source(row_src)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    RegionNode::RegionNode(const RegionNode &rhs)
      : RegionTreeNode(NULL, NULL), handle(LogicalRegion::NO_REGION), 
        parent(NULL), row_source(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    RegionNode::~RegionNode(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    RegionNode& RegionNode::operator=(const RegionNode &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void* RegionNode::operator new(size_t count)
    //--------------------------------------------------------------------------
    {
      return legion_alloc_aligned<RegionNode,true/*bytes*/>(count);
    }

    //--------------------------------------------------------------------------
    void RegionNode::operator delete(void *ptr)
    //--------------------------------------------------------------------------
    {
      free(ptr);
    }

    //--------------------------------------------------------------------------
    bool RegionNode::has_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      return (color_map.find(c) != color_map.end());
    }

    //--------------------------------------------------------------------------
    bool RegionNode::has_color(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      // Ask the row source since it eagerly instantiates
      return row_source->has_child(c);
    }

    //--------------------------------------------------------------------------
    PartitionNode* RegionNode::get_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      // check to see if we have it, if not try to make it
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<ColorPoint,PartitionNode*>::const_iterator finder = 
          color_map.find(c);
        if (finder != color_map.end())
          return finder->second;
      }
      // If we get here we didn't immediately have it so try
      // to make it through the proper channels
      IndexPartNode *index_part = row_source->get_child(c);
#ifdef DEBUG_LEGION
      assert(index_part != NULL);
#endif
      LogicalPartition part_handle(handle.tree_id, index_part->handle,
                                   handle.field_space);
      return context->create_node(part_handle, this);
    }

    //--------------------------------------------------------------------------
    void RegionNode::add_child(PartitionNode *child)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
#ifdef DEBUG_LEGION
      assert(color_map.find(child->row_source->color) == color_map.end());
#endif
      color_map[child->row_source->color] = child;
      valid_map[child->row_source->color] = child;
    }

    //--------------------------------------------------------------------------
    void RegionNode::remove_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      valid_map.erase(c);
    }

    //--------------------------------------------------------------------------
    void RegionNode::add_creation_source(AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      creation_set.add(source);
    }

    //--------------------------------------------------------------------------
    void RegionNode::DestructionFunctor::apply(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      runtime->send_logical_region_destruction(handle, target);
    }

    //--------------------------------------------------------------------------
    void RegionNode::destroy_node(AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      // If we've already been destroyed then we are done
      if (destroyed)
        return;
      bool release_tree_instances = false;
      {
        AutoLock n_lock(node_lock);
        if (!destroyed)
        {
          destroyed = true;
          // If we are the top of the region tree, tell the
          // memories that they can remove any pinned references
          // for any instances in this region tree
          if (parent == NULL)
            release_tree_instances = true;
          if (!creation_set.empty())
          {
            DestructionFunctor functor(handle, context->runtime);
            creation_set.map(functor);
          }
        }
      }
      if (release_tree_instances)
        context->runtime->release_tree_instances(handle.get_tree_id());
    }

    //--------------------------------------------------------------------------
    unsigned RegionNode::get_depth(void) const
    //--------------------------------------------------------------------------
    {
      return row_source->depth;
    }

    //--------------------------------------------------------------------------
    const ColorPoint& RegionNode::get_color(void) const
    //--------------------------------------------------------------------------
    {
      return row_source->color;
    }

    //--------------------------------------------------------------------------
    IndexTreeNode* RegionNode::get_row_source(void) const
    //--------------------------------------------------------------------------
    {
      return row_source;
    }

    //--------------------------------------------------------------------------
    RegionTreeID RegionNode::get_tree_id(void) const
    //--------------------------------------------------------------------------
    {
      return handle.get_tree_id();
    }

    //--------------------------------------------------------------------------
    RegionTreeNode* RegionNode::get_parent(void) const
    //--------------------------------------------------------------------------
    {
      return parent;
    }

    //--------------------------------------------------------------------------
    RegionTreeNode* RegionNode::get_tree_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      return get_child(c);
    }

    //--------------------------------------------------------------------------
    ApEvent RegionNode::issue_copy(Operation *op,
                        const std::vector<Domain::CopySrcDstField> &src_fields,
                        const std::vector<Domain::CopySrcDstField> &dst_fields,
                        ApEvent precondition,RegionTreeNode *intersect/*=NULL*/,
                        ReductionOpID redop /*=0*/,bool reduction_fold/*=true*/)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, REALM_ISSUE_COPY_CALL);
      Realm::ProfilingRequestSet requests;
      if (context->runtime->profiler != NULL)
        context->runtime->profiler->add_copy_request(requests, op);
      ApEvent result;
      if (intersect == NULL)
      {
        // This is a normal copy
        if (row_source->has_component_domains())
        {
          ApEvent dom_pre;
          const std::set<Domain> &doms = 
            row_source->get_component_domains(dom_pre);
          if (dom_pre.exists() && !dom_pre.has_triggered())
            precondition = Runtime::merge_events(precondition, dom_pre);
          std::set<ApEvent> done_events;
          for (std::set<Domain>::const_iterator it = doms.begin();
                it != doms.end(); it++)
          {
            done_events.insert(ApEvent(it->copy(src_fields, dst_fields, 
                                                requests, precondition,
                                                redop, reduction_fold)));
          }
          result = Runtime::merge_events(done_events);
        }
        else
        {
          ApEvent dom_pre;
          const Domain &dom = row_source->get_domain(dom_pre);
          if (dom_pre.exists() && !dom_pre.has_triggered())
            precondition = Runtime::merge_events(precondition, dom_pre);
          result = ApEvent(dom.copy(src_fields, dst_fields, requests, 
                                    precondition, redop, reduction_fold));
        }
      }
      else
      {
        // This is a copy between the intersection of two regions
        const std::set<Domain> *intersection_doms;
        if (intersect->is_region())
          intersection_doms = &(row_source->get_intersection_domains(
                intersect->as_region_node()->row_source));
        else
          intersection_doms = &(row_source->get_intersection_domains(
                intersect->as_partition_node()->row_source));
        std::set<ApEvent> done_events;
        for (std::set<Domain>::const_iterator it = intersection_doms->begin();
              it != intersection_doms->end(); it++)
        {
          done_events.insert(ApEvent(it->copy(src_fields, dst_fields, 
                                              requests, precondition,
                                              redop, reduction_fold)));
        }
        result = Runtime::merge_events(done_events);
      }
#ifdef LEGION_SPY
      if (!result.exists())
      {
        ApUserEvent new_result = Runtime::create_ap_user_event();
        Runtime::trigger_event(new_result);
        result = new_result;
      }
      LegionSpy::log_copy_events(op->get_unique_op_id(), handle, 
                                 precondition, result);
      for (unsigned idx = 0; idx < src_fields.size(); idx++)
        LegionSpy::log_copy_field(result, src_fields[idx].field_id,
                                  src_fields[idx].inst.id,
                                  dst_fields[idx].field_id,
                                  dst_fields[idx].inst.id, redop);
      if (intersect != NULL)
      {
        if (intersect->is_region())
        {
          RegionNode *node = intersect->as_region_node();
          LegionSpy::log_copy_intersect(result, 1, node->handle.index_space.id,
              node->handle.field_space.id, node->handle.tree_id);
        }
        else
        {
          PartitionNode *node = intersect->as_partition_node();
          LegionSpy::log_copy_intersect(result, 0,
              node->handle.index_partition.id,
              node->handle.field_space.id, node->handle.tree_id);
        }
      }
#endif
      return result;
    }

    //--------------------------------------------------------------------------
    ApEvent RegionNode::issue_fill(Operation *op,
                        const std::vector<Domain::CopySrcDstField> &dst_fields,
                        const void *fill_value, size_t fill_size,
                        ApEvent precondition, RegionTreeNode *intersect)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, REALM_ISSUE_FILL_CALL);
      Realm::ProfilingRequestSet requests;
      if (context->runtime->profiler != NULL)
        context->runtime->profiler->add_fill_request(requests, op);
      ApEvent result;
      if (intersect == NULL)
      {
        // This is a normal fill 
        if (row_source->has_component_domains())
        {
          ApEvent dom_pre;
          const std::set<Domain> &doms = 
            row_source->get_component_domains(dom_pre);
          if (dom_pre.exists() && !dom_pre.has_triggered())
            precondition = Runtime::merge_events(precondition, dom_pre);
          std::set<ApEvent> done_events;
          for (std::set<Domain>::const_iterator it = doms.begin();
                it != doms.end(); it++)
          {
            done_events.insert(ApEvent(it->fill(dst_fields, requests, 
                                        fill_value, fill_size, precondition)));
          }
          result = Runtime::merge_events(done_events);
        }
        else
        {
          ApEvent dom_pre;
          const Domain &dom = row_source->get_domain(dom_pre);
          if (dom_pre.exists() && !dom_pre.has_triggered())
            precondition = Runtime::merge_events(precondition, dom_pre);
          result = ApEvent(dom.fill(dst_fields, requests, 
                                    fill_value, fill_size, precondition)); 
        }
      }
      else
      {
        // This is the fill between the intersection of two regions
        const std::set<Domain> *intersection_doms;
        if (intersect->is_region())
          intersection_doms = &(row_source->get_intersection_domains(
                intersect->as_region_node()->row_source));
        else
          intersection_doms = &(row_source->get_intersection_domains(
                intersect->as_partition_node()->row_source));
        std::set<ApEvent> done_events;
        for (std::set<Domain>::const_iterator it = intersection_doms->begin();
              it != intersection_doms->end(); it++)
        {
          done_events.insert(ApEvent(it->fill(dst_fields, requests,
                                      fill_value, fill_size, precondition)));
        }
        result = Runtime::merge_events(done_events);
      }
#ifdef LEGION_SPY
      if (!result.exists())
      {
        ApUserEvent new_result = Runtime::create_ap_user_event();
        Runtime::trigger_event(new_result);
        result = new_result;
      }
      LegionSpy::log_fill_events(op->get_unique_op_id(), handle, 
                                 precondition, result);
      for (unsigned idx = 0; idx < dst_fields.size(); idx++)
        LegionSpy::log_fill_field(result, dst_fields[idx].field_id,
                                  dst_fields[idx].inst.id);
      if (intersect != NULL)
      {
        if (intersect->is_region())
        {
          RegionNode *node = intersect->as_region_node();
          LegionSpy::log_fill_intersect(result, 1, node->handle.index_space.id,
              node->handle.field_space.id, node->handle.tree_id);
        }
        else
        {
          PartitionNode *node = intersect->as_partition_node();
          LegionSpy::log_fill_intersect(result, 0, 
              node->handle.index_partition.id,
              node->handle.field_space.id, node->handle.tree_id);
        }
      }
#endif
      return result;
    }

    //--------------------------------------------------------------------------
    bool RegionNode::are_children_disjoint(const ColorPoint &c1, 
                                           const ColorPoint &c2)
    //--------------------------------------------------------------------------
    {
      return row_source->are_disjoint(c1, c2);
    }

    //--------------------------------------------------------------------------
    bool RegionNode::are_all_children_disjoint(void)
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    void RegionNode::instantiate_children(void)
    //--------------------------------------------------------------------------
    {
      std::set<ColorPoint> all_colors;
      row_source->get_colors(all_colors);
      // This may look like it does nothing, but it checks to see
      // if we have instantiated all the child nodes
      for (std::set<ColorPoint>::const_iterator it = all_colors.begin(); 
            it != all_colors.end(); it++)
        get_child(*it);
    }

    //--------------------------------------------------------------------------
    bool RegionNode::is_region(void) const
    //--------------------------------------------------------------------------
    {
      return true;
    }

#ifdef DEBUG_LEGION
    //--------------------------------------------------------------------------
    RegionNode* RegionNode::as_region_node(void) const
    //--------------------------------------------------------------------------
    {
      return const_cast<RegionNode*>(this);
    }

    //--------------------------------------------------------------------------
    PartitionNode* RegionNode::as_partition_node(void) const
    //--------------------------------------------------------------------------
    {
      return NULL;
    }
#endif

    //--------------------------------------------------------------------------
    AddressSpaceID RegionNode::get_owner_space(void) const
    //--------------------------------------------------------------------------
    {
      return get_owner_space(handle, context->runtime);
    }

    //--------------------------------------------------------------------------
    /*static*/ AddressSpaceID RegionNode::get_owner_space(LogicalRegion handle,
                                                          Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      return (handle.tree_id % runtime->runtime_stride);
    }

    //--------------------------------------------------------------------------
    bool RegionNode::visit_node(PathTraverser *traverser)
    //--------------------------------------------------------------------------
    {
      return traverser->visit_region(this);
    }

    //--------------------------------------------------------------------------
    bool RegionNode::visit_node(NodeTraverser *traverser)
    //--------------------------------------------------------------------------
    {
      bool continue_traversal = traverser->visit_region(this);
      if (continue_traversal)
      {
        const bool break_early = traverser->break_early();
        if (traverser->force_instantiation)
        {
          // If we are forcing instantiation, then grab the set of 
          // colors from the row source and use them to instantiate children
          std::set<ColorPoint> children_colors;
          row_source->get_child_colors(children_colors, 
                                       traverser->visit_only_valid());
          for (std::set<ColorPoint>::const_iterator it = 
                children_colors.begin(); it != children_colors.end(); it++)
          {
            bool result = get_child(*it)->visit_node(traverser);
            continue_traversal = continue_traversal && result;
            if (!result && break_early)
              break;
          }
        }
        else
        {
          std::map<ColorPoint,PartitionNode*> children;
          // Need to hold the lock when reading from 
          // the color map or the valid map
          if (traverser->visit_only_valid())
          {
            AutoLock n_lock(node_lock,1,false/*exclusive*/);
            children = valid_map;
          }
          else
          {
            AutoLock n_lock(node_lock,1,false/*exclusive*/);
            children = color_map;
          }
          for (std::map<ColorPoint,PartitionNode*>::const_iterator it = 
                children.begin(); it != children.end(); it++)
          {
            bool result = it->second->visit_node(traverser);
            continue_traversal = continue_traversal && result;
            if (!result && break_early)
              break;
          }
        }
      }
      return continue_traversal;
    }

    //--------------------------------------------------------------------------
    const Domain& RegionNode::get_domain_blocking(void) const
    //--------------------------------------------------------------------------
    {
      return row_source->get_domain_blocking();
    }

    //--------------------------------------------------------------------------
    const Domain& RegionNode::get_domain(ApEvent &precondition) const
    //--------------------------------------------------------------------------
    {
      return row_source->get_domain(precondition);
    }

    //--------------------------------------------------------------------------
    const Domain& RegionNode::get_domain_no_wait(void) const
    //--------------------------------------------------------------------------
    {
      return row_source->get_domain_no_wait();
    }

    //--------------------------------------------------------------------------
    bool RegionNode::is_complete(void)
    //--------------------------------------------------------------------------
    {
      // For now just assume that regions are never complete
      return false;
    }

    //--------------------------------------------------------------------------
    bool RegionNode::intersects_with(RegionTreeNode *other)
    //--------------------------------------------------------------------------
    {
      if (other == this)
        return true;
      if (other->is_region())
        return row_source->intersects_with(
                  other->as_region_node()->row_source);
      else
        return row_source->intersects_with(
                  other->as_partition_node()->row_source);
    }

    //--------------------------------------------------------------------------
    bool RegionNode::dominates(RegionTreeNode *other)
    //--------------------------------------------------------------------------
    {
      if (other == this)
        return true;
      if (other->is_region())
        return row_source->dominates(other->as_region_node()->row_source);
      else
        return row_source->dominates(other->as_partition_node()->row_source);
    }

    //--------------------------------------------------------------------------
    size_t RegionNode::get_num_children(void) const
    //--------------------------------------------------------------------------
    {
      return row_source->get_num_children();
    }

    //--------------------------------------------------------------------------
    InterCloseOp* RegionNode::create_close_op(Operation *creator,
                                              const FieldMask &closing_mask,
                        const LegionMap<ColorPoint,FieldMask>::aligned &targets,
                                              const VersionInfo &close_info,
                                              const VersionInfo &version_info,
                                              const TraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
      InterCloseOp *op = context->runtime->get_available_inter_close_op(false);
      // Construct a reigon requirement for this operation
      // All privileges are based on the parent logical region
      RegionRequirement req(handle, READ_WRITE, EXCLUSIVE, 
                            trace_info.req.parent);
      // Compute the set of fields that we need
      column_source->get_field_set(closing_mask, 
                                   trace_info.req.privilege_fields,
                                   req.privilege_fields);
      // Now initialize the operation
      op->initialize(creator->get_parent(), req, targets, 
                     trace_info.trace, trace_info.req_idx, 
                     close_info, version_info, closing_mask, creator);
      return op;
    }

    //--------------------------------------------------------------------------
    ReadCloseOp* RegionNode::create_read_only_close_op(Operation *creator,
                                            const FieldMask &closing_mask,
                        const LegionMap<ColorPoint,FieldMask>::aligned &targets,
                                            const TraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
      ReadCloseOp *op = context->runtime->get_available_read_close_op(false);
      // Construct a reigon requirement for this operation
      // All privileges are based on the parent logical region
      RegionRequirement req(handle, READ_WRITE, EXCLUSIVE, 
                            trace_info.req.parent);
      // Compute the set of fields that we need
      column_source->get_field_set(closing_mask, 
                                   trace_info.req.privilege_fields,
                                   req.privilege_fields);
      // Now initialize the operation
      op->initialize(creator->get_parent(), req, targets, trace_info.trace,
                     trace_info.req_idx, closing_mask, creator);
      return op;
    }

    //--------------------------------------------------------------------------
    void RegionNode::send_node(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      // Check to see if we have it in our creation set in which
      // case we are done otherwise keep going up
      bool continue_up = false;
      bool send_deletion = false;
      {
        AutoLock n_lock(node_lock); 
        if (!creation_set.contains(target))
        {
          continue_up = true;
          creation_set.add(target);
        }
        if (destroyed)
          send_deletion = true;
      }
      if (continue_up)
      {
        if (parent != NULL)
        {
          // Send the parent node first
          parent->send_node(target);
          AutoLock n_lock(node_lock);
          for (LegionMap<SemanticTag,SemanticInfo>::aligned::iterator it = 
                semantic_info.begin(); it != semantic_info.end(); it++)
          {
            Serializer rez;
            {
              RezCheck z(rez);
              rez.serialize(handle);
              rez.serialize(it->first);
              rez.serialize(it->second.size);
              rez.serialize(it->second.buffer, it->second.size);
              rez.serialize(it->second.is_mutable);
            }
            context->runtime->send_logical_region_semantic_info(target, rez);
          }
        }
        else
        {
          // We've made it to the top, send this node
          Serializer rez;
          {
            RezCheck z(rez);
            rez.serialize(handle);
            rez.serialize<size_t>(semantic_info.size());
            for (LegionMap<SemanticTag,SemanticInfo>::aligned::iterator it = 
                  semantic_info.begin(); it != semantic_info.end(); it++)
            {
              rez.serialize(it->first);
              rez.serialize(it->second.size);
              rez.serialize(it->second.buffer, it->second.size);
              rez.serialize(it->second.is_mutable);
            }
          }
          context->runtime->send_logical_region_node(target, rez);
        }
      }
      if (send_deletion)
        context->runtime->send_logical_region_destruction(handle, target);
    }

    //--------------------------------------------------------------------------
    /*static*/ void RegionNode::handle_node_creation(RegionTreeForest *context,
                                     Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      LogicalRegion handle;
      derez.deserialize(handle);

      RegionNode *node = context->create_node(handle, NULL/*parent*/);
#ifdef DEBUG_LEGION
      assert(node != NULL);
#endif
      node->add_creation_source(source);
      size_t num_semantic;
      derez.deserialize(num_semantic);
      if (num_semantic > 0)
      {
        NodeSet source_mask;
        source_mask.add(source);
        source_mask.add(context->runtime->address_space);
        for (unsigned idx = 0; idx < num_semantic; idx++)
        {
          SemanticTag tag;
          derez.deserialize(tag);
          size_t buffer_size;
          derez.deserialize(buffer_size);
          const void *buffer = derez.get_current_pointer();
          derez.advance_pointer(buffer_size);
          bool is_mutable;
          derez.deserialize(is_mutable);
          node->attach_semantic_information(tag, source, 
                                            buffer, buffer_size, is_mutable);
        }
      }
    } 

    //--------------------------------------------------------------------------
    CompositeView* RegionNode::map_virtual_region(ContextID ctx_id,
                                                 const FieldMask &virtual_mask,
                                                 VersionInfo &version_info,
                                                 SingleTask *target_ctx)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, REGION_NODE_MAP_VIRTUAL_CALL);
      PhysicalState *state = get_physical_state(version_info);
      // Figure out which children we need to close
      LegionMap<ColorPoint,FieldMask>::aligned targets;
      std::set<ColorPoint> next;
      if (!(virtual_mask * state->children.valid_fields))
      {
        LegionMap<ColorPoint,FieldMask>::aligned &open_children = 
          state->children.open_children;
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it = 
              open_children.begin(); it != open_children.end(); it++)
        {
          if (it->second * virtual_mask)
            continue;
          targets[it->first] = FieldMask();
        }
      }
      return create_composite_instance(ctx_id, targets, next, virtual_mask, 
                                       version_info, target_ctx);
    }

    //--------------------------------------------------------------------------
    void RegionNode::register_region(const TraversalInfo &info,
                                     RestrictInfo &restrict_info,
                                   ApEvent term_event, const RegionUsage &usage,
                                   bool defer_add_users, InstanceSet &targets)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, REGION_NODE_REGISTER_REGION_CALL);
      PhysicalState *state = get_physical_state(info.version_info);
      const AddressSpaceID local_space = context->runtime->address_space;
      // Get any restricted fields
      FieldMask restricted_fields;
      InstanceSet restricted_instances;
      std::vector<MaterializedView*> restricted_views;
      if (restrict_info.has_restrictions())
      {
        restrict_info.populate_restrict_fields(restricted_fields);
        // We only need to get these things if we are not read-only
        if (!IS_READ_ONLY(info.req))
        {
          restricted_instances = restrict_info.get_instances();
          restricted_views.resize(restricted_instances.size());
          convert_target_views(restricted_instances, 
                               info.op->get_parent(), restricted_views);
        }
      }
#ifdef DEBUG_LEGION
      for (unsigned idx = 0; idx < targets.size(); idx++)
      {
        const InstanceRef &ref = targets[idx];
        const FieldMask &valid_fields = ref.get_valid_fields();
        assert(unsigned(FieldMask::pop_count(valid_fields)) <= 
                              info.req.privilege_fields.size());
      }
#endif
      if (IS_REDUCE(info.req))
      {
        // Reduction only case
        std::vector<ReductionView*> new_views;
        if (!defer_add_users && (targets.size() > 1))
          new_views.resize(targets.size());
        LegionMap<ReductionView*,FieldMask>::aligned reduce_out_views;
        for (unsigned idx = 0; idx < targets.size(); idx++)
        {
          InstanceRef &ref = targets[idx];
#ifdef DEBUG_LEGION
          assert(!ref.is_composite_ref());
#endif
          LogicalView *view = convert_reference(ref, info.op->get_parent());
#ifdef DEBUG_LEGION
          assert(view->is_instance_view());
          assert(view->as_instance_view()->is_reduction_view());
#endif
          ReductionView *new_view = 
            view->as_instance_view()->as_reduction_view();
          const FieldMask &user_mask = ref.get_valid_fields(); 
          // Only add reductions which are not restricted
          if (!!restricted_fields)
          {
            FieldMask unrestricted = user_mask - restricted_fields;
            if (!!unrestricted)
              update_reduction_views(state, unrestricted, new_view);
          }
          else // the normal path here
            update_reduction_views(state, user_mask, new_view);
          // Skip adding the user if requested
          if (defer_add_users || (targets.size() > 1))
            continue;
          if (targets.size() > 1)
          {
            // Only find the preconditions now 
            ApEvent ready = new_view->find_user_precondition(usage, term_event,
                                   user_mask, info.op, info.index, 
                                   info.version_info, info.map_applied_events);
            ref.set_ready_event(ready);
            new_views[idx] = new_view;
          }
          else
          {
            // Do the fused find preconditions and add user
            ApEvent ready = new_view->add_user_fused(usage,term_event,user_mask,
                                         info.op, info.index, info.version_info,
                                         local_space, info.map_applied_events);
            ref.set_ready_event(ready);
          }
          if (!defer_add_users && !!restricted_fields)
          {
            FieldMask restricted = restricted_fields & user_mask;
              if (!!restricted)
                reduce_out_views[new_view] = restricted;
          }
        }
        if (!defer_add_users && (targets.size() > 1))
        {
          // Second pass of the two pass approach, add our users
          for (unsigned idx = 0; idx < targets.size(); idx++)
          {
            InstanceRef &ref = targets[idx]; 
            new_views[idx]->add_user(usage, term_event, ref.get_valid_fields(),
                                   info.op, info.index, local_space,
                                   info.version_info, info.map_applied_events);
          }
        }
        if (!reduce_out_views.empty())
          issue_restricted_reductions(info, restricted_instances, 
                                      restricted_views, reduce_out_views);
      }
      else
      {
        // Normal instances
        std::vector<InstanceView*> new_views(targets.size());
        convert_target_views(targets, info.op->get_parent(), new_views);
        if (!IS_WRITE_ONLY(info.req))
        {
          // Any case but write-only
          // All close operations have already been done, so all
          // we need to do is bring our instances up to date for
          // any of their missing fields, and then register our views

          // Finding the valid views is expensive so hold off doing
          // it at first, but as soon as we need it for any instance
          // then do it for all of them and remember the result.
          LegionMap<LogicalView*,FieldMask>::aligned valid_views;
          bool has_valid_views = false;
          for (unsigned idx = 0; idx < targets.size(); idx++)
          {
            const InstanceRef &ref = targets[idx];
#ifdef DEBUG_LEGION
            assert(!ref.is_composite_ref());
            assert(new_views[idx]->is_instance_view());
            assert(new_views[idx]->as_instance_view()->is_materialized_view());
#endif
            const FieldMask &valid_fields = ref.get_valid_fields();
            MaterializedView *view = 
              new_views[idx]->as_instance_view()->as_materialized_view();
            // See if this instance is valid already 
            LegionMap<LogicalView*,FieldMask>::aligned::const_iterator
              finder = state->valid_views.find(view);
            if (finder != state->valid_views.end())
            {
              // See which fields if any we actually need to update
              FieldMask needed_fields = valid_fields - finder->second;
              if (!!needed_fields)
              {
                if (!has_valid_views)
                {
                  find_valid_instance_views(info.ctx, state, 
                      info.traversal_mask, info.traversal_mask, 
                      info.version_info, false/*needs space*/, valid_views);
                  has_valid_views = true;
                }
                issue_update_copies(info, view, needed_fields, valid_views);
              }
            }
            else
            {
              // Not valid for any fields, so bring it up to date for all fields
              if (!has_valid_views)
              {
                find_valid_instance_views(info.ctx, state,
                    info.traversal_mask, info.traversal_mask,
                    info.version_info, false/*needs space*/, valid_views);
                has_valid_views = true;
              }
              issue_update_copies(info, view, valid_fields, valid_views);
            }
            // Finally add this to the set of valid views for the state
            if (!!restricted_fields)
            {
              // We can only update instances which aren't restricted
              FieldMask unrestricted = valid_fields - restricted_fields;
              if (!!unrestricted)
                update_valid_views(state, unrestricted,
                                   !IS_READ_ONLY(info.req)/*dirty*/, view);
            }
            else // The normal path
              update_valid_views(state, valid_fields, 
                                 !IS_READ_ONLY(info.req)/*dirty*/, view); 
          }
        }
        else
        {
          // Write-only case
          // Remove any open children for these fields, we don't
          // need to traverse down the tree because we already advanced
          // those version numbers logically so we don't need to be 
          // updating the version states
          if (!(info.traversal_mask * state->children.valid_fields))
            state->filter_open_children(info.traversal_mask);
          // Remove any overlapping reducitons
          if (!(info.traversal_mask * state->reduction_mask))
            invalidate_reduction_views(state, info.traversal_mask);
          // This is write-only so update the valid views on the
          // state with the new instance views while invalidating
          // any old instances with views that interfere with the
          // dirty mask
          if (!!restricted_fields)
          {
            // If we have restricted fields, we have to update them
            // one by one to make sure that we only add instances
            // which aren't restricted
            for (unsigned idx = 0; idx < targets.size(); idx++)
            {
              const InstanceRef &ref = targets[idx];
              FieldMask unrestricted = 
                ref.get_valid_fields() - restricted_fields;
              if (!!unrestricted)
                update_valid_views(state, unrestricted,
                                   true/*dirty*/, new_views[idx]); 
            }
          }
          else // The normal path
            update_valid_views(state, info.traversal_mask, 
                               new_views, targets);
        }
        // Finally we have to update our instance references
        // to get the ready events
        if (!defer_add_users)
        {
          // If there is exactly one instance, then we can do
          // the fused analysis and register user, otherwise we
          // have to make two passes in order to avoid event cycles 
          LegionMap<LogicalView*,FieldMask>::aligned copy_out_views;
          if (targets.size() == 1)
          {
            InstanceRef &ref = targets[0];
#ifdef DEBUG_LEGION
            assert(!ref.is_composite_ref());
            assert(new_views[0]->is_instance_view());
#endif
            ApEvent ready = new_views[0]->as_instance_view()->add_user_fused(
                usage, term_event, ref.get_valid_fields(), info.op, info.index,
                info.version_info, local_space, info.map_applied_events);
            ref.set_ready_event(ready);
            if (!!restricted_fields)
            {
              FieldMask restricted = ref.get_valid_fields() & restricted_fields;
              if (!!restricted)
                copy_out_views[new_views[0]] = restricted;
            }
          }
          else
          {
            // Two pass approach to avoid event cycles 
            for (unsigned idx = 0; idx < targets.size(); idx++)
            {
              InstanceRef &ref = targets[idx];
#ifdef DEBUG_LEGION
              assert(!ref.is_composite_ref());
              assert(new_views[idx]->is_instance_view());
#endif
              ApEvent ready = 
                new_views[idx]->as_instance_view()->find_user_precondition(
                    usage, term_event, ref.get_valid_fields(), info.op, 
                    info.index, info.version_info, info.map_applied_events);
              ref.set_ready_event(ready);
            }
            for (unsigned idx = 0; idx < targets.size(); idx++)
            {
              InstanceRef &ref = targets[idx];
              new_views[idx]->as_instance_view()->add_user(usage, term_event,
                       ref.get_valid_fields(), info.op, info.index, 
                       local_space, info.version_info, info.map_applied_events);
              if (!!restricted_fields)
              {
                FieldMask restricted = 
                  ref.get_valid_fields() & restricted_fields;
                if (!!restricted)
                  copy_out_views[new_views[idx]] = restricted;
              }
            }
          }
          // If we have to do any copy out operations, then we do that now
          if (!copy_out_views.empty())
            issue_restricted_copies(info, restricted_instances, 
                                    restricted_views, copy_out_views);
        }
      }
      // If we have any restricted instances, we can now update the state
      // to reflect that they are going to be the valid instances
      if (!!restricted_fields && !IS_READ_ONLY(info.req))
        update_valid_views(state, restricted_fields, 
                           restricted_views, restricted_instances);
    }

    //--------------------------------------------------------------------------
    void RegionNode::register_virtual(ContextID ctx, const InstanceRef &ref,
                     VersionInfo &version_info, const FieldMask &composite_mask)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(ref.is_composite_ref());
#endif
      // Convert the view locally
      CompositeView *view = convert_reference(ref);
      PhysicalState *state = get_physical_state(version_info);
      update_valid_views(state, composite_mask, true/*dirty*/, view);
    }

    //--------------------------------------------------------------------------
    void RegionNode::seed_state(ContextID ctx, ApEvent term_event,
                                const RegionUsage &usage,
                                const FieldMask &user_mask,
                                const InstanceSet &targets,
                                SingleTask *context, unsigned init_index,
                                const std::vector<LogicalView*> &corresponding)
    //--------------------------------------------------------------------------
    {
      get_current_state(ctx).initialize_state(term_event, usage, user_mask, 
                              targets, context, init_index, corresponding);
    } 

    //--------------------------------------------------------------------------
    void RegionNode::close_state(const TraversalInfo &info, RegionUsage &usage, 
                                 InstanceSet &targets)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(context->runtime, REGION_NODE_CLOSE_STATE_CALL);
      if (IS_REDUCE(info.req))
      {
        PhysicalState *state = get_physical_state(info.version_info);
        // Important trick: switch the user to read-only so it picks
        // up dependences on all the reductions applied to this instance
        usage.privilege = READ_ONLY;
        const AddressSpaceID local_space = context->runtime->address_space;
        for (unsigned idx = 0; idx < targets.size(); idx++)
        {
          InstanceRef &ref = targets[idx];
          LogicalView *view = convert_reference(ref, info.op->get_parent());
#ifdef DEBUG_LEGION
          assert(view->is_instance_view());
          assert(view->as_instance_view()->is_reduction_view());
#endif
          ReductionView *target_view = 
            view->as_instance_view()->as_reduction_view();
          const FieldMask &user_mask = ref.get_valid_fields();
          // Flush any reductions from this level, and then flush any
          // from farther down in the tree
          ReductionCloser closer(info.ctx, target_view, user_mask, 
             info.version_info, info.op, info.index, info.map_applied_events);
          closer.issue_close_reductions(this, state);
          siphon_physical_children(closer, state);
          
          ApEvent ready = target_view->add_user_fused(
                                               usage, ApEvent::NO_AP_EVENT,
                                               user_mask, info.op, info.index,
                                               info.version_info, local_space, 
                                               info.map_applied_events);
          ref.set_ready_event(ready);
        }
      }
      else
      {
        // All we should actually have to do here is just register
        // our region because any actualy close operations that would
        // need to be done would have been issued as part of the 
        // logical analysis or will be done as part of the registration.
        RestrictInfo empty_restrict_info;
        register_region(info, empty_restrict_info, ApEvent::NO_AP_EVENT, 
                        usage, false/*defer add users*/, targets);
      }
    }

    //--------------------------------------------------------------------------
    void RegionNode::find_field_descriptors(ContextID ctx, ApEvent term_event,
                                            const RegionUsage &usage,
                                            const FieldMask &user_mask,
                                            FieldID field_id, Operation *op,
                                            unsigned index,
                                  std::vector<FieldDataDescriptor> &field_data,
                                            std::set<ApEvent> &preconditions,
                                            VersionInfo &version_info,
                                            std::set<RtEvent> &applied_events)
    //--------------------------------------------------------------------------
    {
      PhysicalState *state = get_physical_state(version_info);
      // First pull down any valid instance views
      pull_valid_instance_views(ctx, state, user_mask, 
                                false/*need space*/, version_info);
      // Now go through the list of valid instances and see if we can find
      // one that satisfies the field that we need.
      DeferredView *deferred_view = NULL;
      const AddressSpaceID local_space = context->runtime->address_space;
      for (LegionMap<LogicalView*,FieldMask>::track_aligned::const_iterator
            it = state->valid_views.begin(); 
            it != state->valid_views.end(); it++)
      {
        // Check to see if the instance is valid for our target field
        if (!!(it->second & user_mask))
        {
          // See if this is a composite view or not
          if (it->first->is_instance_view())
          {
#ifdef DEBUG_LEGION
            assert(it->first->as_instance_view()->is_materialized_view());
#endif
            MaterializedView *view = 
              it->first->as_instance_view()->as_materialized_view(); 
            // Record the instance and its information
            field_data.push_back(FieldDataDescriptor());
            view->set_descriptor(field_data.back(), field_id);
            // Register ourselves as user of this instance
            ApEvent ready_event = view->add_user_fused(usage, term_event, 
              user_mask, op, index, version_info, local_space, applied_events);
            if (ready_event.exists())
              preconditions.insert(ready_event);
            // We found an actual instance so we are done
            deferred_view = NULL;
            break;
          }
          else
          {
            // Save it as a composite view and keep going
#ifdef DEBUG_LEGION
            assert(it->first->is_deferred_view());
            // There should be at most one composite view for this field
            assert(deferred_view == NULL);
#endif
            deferred_view = it->first->as_deferred_view();
          }
        }
      }
      if (deferred_view != NULL)
      {
        // If this is a fill view, we either need to make a new physical
        // instance or apply it to all the existing physical instances
        if (!deferred_view->is_composite_view())
          assert(false); // TODO: implement this
        deferred_view->find_field_descriptors(term_event, usage, 
                                              user_mask, field_id, op, index,
                                              field_data, preconditions);
      }
    }

    //--------------------------------------------------------------------------
    void RegionNode::fill_fields(ContextID ctx, const FieldMask &fill_mask,
                                 const void *value, size_t value_size,
                                 VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
      // Make the fill instance
      DistributedID did = context->runtime->get_available_distributed_id(false);
      FillView::FillViewValue *fill_value = 
        new FillView::FillViewValue(value, value_size);
      FillView *fill_view = 
        legion_new<FillView>(context, did, context->runtime->address_space,
                             context->runtime->address_space, this, 
                             fill_value, true/*register now*/);
      // Now update the physical state
      PhysicalState *state = get_physical_state(version_info);
      // Invalidate any open children and any reductions
      if (!(fill_mask * state->children.valid_fields))
        state->filter_open_children(fill_mask); 
      if (!(fill_mask * state->reduction_mask))
        invalidate_reduction_views(state, fill_mask);
      update_valid_views(state, fill_mask, true/*dirty*/, fill_view);
    }

    //--------------------------------------------------------------------------
    ApEvent RegionNode::eager_fill_fields(ContextID ctx, Operation *op,
                                          const unsigned index,
                                          const FieldMask &fill_mask,
                                          const void *value, size_t value_size,
                                          VersionInfo &version_info, 
                                          InstanceSet &instances,
                                          ApEvent sync_precondition,
                                          std::set<RtEvent> &map_applied_events)
    //--------------------------------------------------------------------------
    {
      // Effectively a fill is a special kind of copy so we can analyze
      // it the same way to figure out how to issue the fill
      std::set<ApEvent> post_events;
      std::vector<InstanceView*> target_views(instances.size(), NULL);
      convert_target_views(instances, op->get_parent(), target_views);
      const AddressSpaceID local_space = context->runtime->address_space;
      for (unsigned idx = 0; idx < instances.size(); idx++)
      {
        InstanceView *target = target_views[idx];
        LegionMap<ApEvent,FieldMask>::aligned preconditions;
        target->find_copy_preconditions(0/*redop*/, false/*reading*/, fill_mask,
                                version_info, op->get_unique_op_id(), index,
                                local_space, preconditions, map_applied_events);
        if (sync_precondition.exists())
          preconditions[sync_precondition] = fill_mask;
        // Sort the preconditions into event sets
        LegionList<EventSet>::aligned event_sets;
        compute_event_sets(fill_mask, preconditions, event_sets);
        // Iterate over the event sets and issue the fill operations on 
        // the different fields
        for (LegionList<EventSet>::aligned::const_iterator pit = 
              event_sets.begin(); pit != event_sets.end(); pit++)
        {
          ApEvent precondition = Runtime::merge_events(pit->preconditions);
          std::vector<Domain::CopySrcDstField> dst_fields;
          target->copy_to(pit->set_mask, dst_fields);
          ApEvent fill_event = issue_fill(op, dst_fields, 
                                          value, value_size, precondition);
          if (fill_event.exists())
            post_events.insert(fill_event);
        }
        // Add user to make record when everyone is done writing
        target->add_copy_user(0/*redop*/, op->get_completion_event(), 
                              version_info, op->get_unique_op_id(), 
                              index, fill_mask, false/*reading*/,
                              local_space, map_applied_events);
      }
      // Finally do the update to the physical state like a normal fill
      PhysicalState *state = get_physical_state(version_info);
      // Invalidate any open children and any reductions
      if (!(fill_mask * state->children.valid_fields))
        state->filter_open_children(fill_mask); 
      if (!(fill_mask * state->reduction_mask))
        invalidate_reduction_views(state, fill_mask);
      update_valid_views(state, fill_mask, target_views, instances);
      // Return the merge of all the post events
      return Runtime::merge_events(post_events);
    }

    //--------------------------------------------------------------------------
    InstanceRef RegionNode::attach_file(ContextID ctx, SingleTask *parent_ctx, 
                                        const FieldMask &attach_mask,
                                        const RegionRequirement &req,
                                        InstanceManager *manager,
                                        VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
      // Wrap it in a view
      MaterializedView *view = parent_ctx->create_instance_top_view(manager, 
            context->runtime->address_space)->as_materialized_view();
#ifdef DEBUG_LEGION
      assert(view != NULL);
#endif
      ApUserEvent ready_event = Runtime::create_ap_user_event();
      // Update the physical state with the new instance
      PhysicalState *state = get_physical_state(version_info);
      // We need to invalidate all other instances for these fields since
      // we are now making this the only valid copy of the data
      update_valid_views(state, attach_mask, true/*dirty*/, view);
      // Return the resulting instance
      return InstanceRef(manager, attach_mask, ready_event);
    }

    //--------------------------------------------------------------------------
    ApEvent RegionNode::detach_file(ContextID ctx, DetachOp *detach_op, 
                                    VersionInfo &version_info, 
                                    const InstanceRef &ref)
    //--------------------------------------------------------------------------
    {
      InstanceView *view = convert_reference(ref, detach_op->get_parent());
#ifdef DEBUG_LEGION
      assert(view->is_materialized_view());
#endif
      MaterializedView *detach_view = view->as_materialized_view();
      // First remove this view from the set of valid views
      PhysicalState *state = get_physical_state(version_info);
      filter_valid_views(state, detach_view);
      return ApEvent::NO_AP_EVENT;
    }

    //--------------------------------------------------------------------------
    InstanceView* RegionNode::find_context_view(PhysicalManager *manager,
                                                SingleTask *context)
    //--------------------------------------------------------------------------
    {
      return convert_reference_region(manager, context);
    }

    //--------------------------------------------------------------------------
    InstanceView* RegionNode::convert_reference_region(PhysicalManager *manager,
                                                       SingleTask *context)
    //--------------------------------------------------------------------------
    {
      InstanceView *result = find_instance_view(manager, context);
      if (result != NULL)
        return result;
      if (manager->region_node == this)
        result = context->create_instance_top_view(manager,
                                              context->runtime->address_space);
      else
      {
#ifdef DEBUG_LEGION
        assert(parent != NULL);
#endif
        InstanceView *parent_view = 
          parent->convert_reference_partition(manager, context);
        result = parent_view->get_instance_subview(row_source->color);
      }
      return result;
    }

    //--------------------------------------------------------------------------
    CompositeView* RegionNode::convert_composite_view_region(
                                                      CompositeView *view) const
    //--------------------------------------------------------------------------
    {
      if (view->logical_node == this)
        return view;
#ifdef DEBUG_LEGION
      assert(parent != NULL);
#endif
      CompositeView *parent_view = 
        parent->convert_composite_view_partition(view);
      return parent_view->get_subview(row_source->color)->
                                as_deferred_view()->as_composite_view();
    }

    //--------------------------------------------------------------------------
    void RegionNode::convert_references_region(
                                const std::vector<PhysicalManager*> &managers,
                                std::vector<bool> &up_mask, SingleTask *context,
                                std::vector<InstanceView*> &results)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(managers.size() == up_mask.size());
#endif
      // See which ones are still good here
      std::vector<unsigned> up_indexes;
      for (unsigned idx = 0; idx < managers.size(); idx++)
      {
        if (!up_mask[idx]) // skip any that are no longer valid
          continue;
        // Check to see if we already have it
        PhysicalManager *manager = managers[idx];
        results[idx] = find_instance_view(manager, context);
        // If we've got it we are done with it
        if (results[idx] != NULL)
        {
          // Mark that it is done
          up_mask[idx] = false;
          continue;
        }
        if (manager->region_node == this)
        {
          results[idx] = 
            context->create_instance_top_view(manager, 
                      context->runtime->address_space);
          // Mark that it is done
          up_mask[idx] = false;
        }
        else
          up_indexes.push_back(idx);
      }
      // See if we have to keep going up
      if (!up_indexes.empty())
      {
#ifdef DEBUG_LEGION
        assert(parent != NULL);
#endif
        parent->convert_references_partition(managers, up_mask, 
                                             context, results);
        for (unsigned idx = 0; idx < up_indexes.size(); idx++)
        {
          unsigned index = up_indexes[idx];
          InstanceView *parent_view = results[index];
          results[index] = parent_view->get_instance_subview(row_source->color);
        }
      }
    }

    //--------------------------------------------------------------------------
    void RegionNode::send_semantic_request(AddressSpaceID target,
             SemanticTag tag, bool can_fail, bool wait_until, RtUserEvent ready)
    //--------------------------------------------------------------------------
    {
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(tag);
        rez.serialize(can_fail);
        rez.serialize(wait_until);
        rez.serialize(ready);
      }
      context->runtime->send_logical_region_semantic_request(target, rez);
    }

    //--------------------------------------------------------------------------
    void RegionNode::send_semantic_info(AddressSpaceID target,
                                        SemanticTag tag,
                                        const void *buffer, size_t size, 
                                        bool is_mutable)
    //--------------------------------------------------------------------------
    {
      // Package up the message first
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(tag);
        rez.serialize(size);
        rez.serialize(buffer, size);
        rez.serialize(is_mutable);
      }
      context->runtime->send_logical_region_semantic_info(target, rez);
    }

    //--------------------------------------------------------------------------
    void RegionNode::process_semantic_request(SemanticTag tag,
       AddressSpaceID source, bool can_fail, bool wait_until, RtUserEvent ready)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(get_owner_space() == context->runtime->address_space);
#endif
      RtEvent precondition;
      void *result = NULL;
      size_t size = 0;
      bool is_mutable = false;
      {
        AutoLock n_lock(node_lock);
        // See if we already have the data
        LegionMap<SemanticTag,SemanticInfo>::aligned::iterator finder = 
          semantic_info.find(tag);
        if (finder != semantic_info.end())
        {
          if (finder->second.is_valid())
          {
            result = finder->second.buffer;
            size = finder->second.size;
            is_mutable = finder->second.is_mutable;
          }
          else if (!can_fail && wait_until)
            precondition = finder->second.ready_event;
        }
        else if (!can_fail && wait_until)
        {
          // Don't have it yet, make a condition and hope that one comes
          RtUserEvent ready_event = Runtime::create_rt_user_event();
          precondition = ready_event;
          semantic_info[tag] = SemanticInfo(ready_event);
        }
      }
      if (result == NULL)
      {
        if (can_fail || !wait_until)
          Runtime::trigger_event(ready);
        else
        {
          // Defer this until the semantic condition is ready
          SemanticRequestArgs args;
          args.hlr_id = HLR_REGION_SEMANTIC_INFO_REQ_TASK_ID;
          args.proxy_this = this;
          args.tag = tag;
          args.source = source;
          context->runtime->issue_runtime_meta_task(&args, sizeof(args),
                              HLR_REGION_SEMANTIC_INFO_REQ_TASK_ID,
                              HLR_LATENCY_PRIORITY, NULL/*op*/, precondition);
        }
      }
      else
        send_semantic_info(source, tag, result, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    /*static*/ void RegionNode::handle_semantic_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      LogicalRegion handle;
      derez.deserialize(handle);
      SemanticTag tag;
      derez.deserialize(tag);
      bool can_fail;
      derez.deserialize(can_fail);
      bool wait_until;
      derez.deserialize(wait_until);
      RtUserEvent ready;
      derez.deserialize(ready);
      RegionNode *node = forest->get_node(handle);
      node->process_semantic_request(tag, source, can_fail, wait_until, ready);
    }

    //--------------------------------------------------------------------------
    /*static*/ void RegionNode::handle_semantic_info(RegionTreeForest *forest,
                                     Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      LogicalRegion handle;
      derez.deserialize(handle);
      SemanticTag tag;
      derez.deserialize(tag);
      size_t size;
      derez.deserialize(size);
      const void *buffer = derez.get_current_pointer();
      derez.advance_pointer(size);
      bool is_mutable;
      derez.deserialize(is_mutable);
      forest->attach_semantic_information(handle, tag, source, 
                                          buffer, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    /*static*/ void RegionNode::handle_top_level_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      RegionTreeID tid;
      derez.deserialize(tid);
      RegionNode *node = forest->get_tree(tid);
      node->send_node(source);
      RtUserEvent done_event;
      derez.deserialize(done_event);
      Serializer rez;
      rez.serialize(done_event);
      forest->runtime->send_top_level_region_return(source, rez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void RegionNode::handle_top_level_return(Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      RtUserEvent done_event;
      derez.deserialize(done_event);
      Runtime::trigger_event(done_event);
    }

    //--------------------------------------------------------------------------
    void RegionNode::print_logical_context(ContextID ctx, 
                                           TreeStateLogger *logger,
                                           const FieldMask &capture_mask) 
    //--------------------------------------------------------------------------
    {
      switch (row_source->color.get_dim())
      {
        case 0:
          {
            logger->log("Region Node (%x,%d,%d) Color %d at depth %d", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color.get_index(), logger->get_depth());
            break;
          }
        case 1:
          {
            logger->log("Region Node (%x,%d,%d) Color %d at depth %d", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], logger->get_depth());
            break;
          }
        case 2:
          {
            logger->log("Region Node (%x,%d,%d) Color (%d,%d) at "
                        "depth %d", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], 
              row_source->color[1], logger->get_depth());
            break;
          }
        case 3:
          {
            logger->log("Region Node (%x,%d,%d) Color (%d,%d,%d) at "
                        "depth %d", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->color[2],
              row_source->color[2], logger->get_depth());
            break;
          }
        default:
          assert(false);
      }
      logger->down();
      LegionMap<ColorPoint,FieldMask>::aligned to_traverse;
      if (current_states.has_entry(ctx))
      {
        CurrentState &state = get_current_state(ctx);
        print_logical_state(state, capture_mask, to_traverse, logger);  
      }
      else
      {
        logger->log("No state");
      }
      logger->log("");
      if (!to_traverse.empty())
      {
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it =
              to_traverse.begin(); it != to_traverse.end(); it++)
        {
          std::map<ColorPoint,PartitionNode*>::const_iterator finder = 
            color_map.find(it->first);
          if (finder != color_map.end())
            finder->second->print_logical_context(ctx, logger, it->second);
        }
      }
      logger->up();
    }

    //--------------------------------------------------------------------------
    void RegionNode::print_physical_context(ContextID ctx, 
                                            TreeStateLogger *logger,
                                            const FieldMask &capture_mask)
    //--------------------------------------------------------------------------
    {
      switch (row_source->color.get_dim())
      {
        case 0:
          {
            logger->log("Region Node (%x,%d,%d) Color %d at depth %d", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color.get_index(), logger->get_depth());
            break;
          }
        case 1:
          {
            logger->log("Region Node (%x,%d,%d) Color %d at depth %d", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], logger->get_depth());
            break;
          }
        case 2:
          {
            logger->log("Region Node (%x,%d,%d) Color (%d,%d) at "
                        "depth %d", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], 
              row_source->color[1], logger->get_depth());
            break;
          }
        case 3:
          {
            logger->log("Region Node (%x,%d,%d) Color (%d,%d,%d) at "
                        "depth %d", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->color[2],
              row_source->color[2], logger->get_depth());
            break;
          }
        default:
          assert(false);
      }
      logger->down();
      LegionMap<ColorPoint,FieldMask>::aligned to_traverse;
      if (current_states.has_entry(ctx))
      {
        CurrentState &state = get_current_state(ctx);
        state.print_physical_state(this, capture_mask, to_traverse, logger);
      }
      else
      {
        logger->log("No state");
      }
      logger->log("");
      if (!to_traverse.empty())
      {
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it =
              to_traverse.begin(); it != to_traverse.end(); it++)
        {
          std::map<ColorPoint,PartitionNode*>::const_iterator finder = 
            color_map.find(it->first);
          if (finder != color_map.end())
            finder->second->print_physical_context(ctx, logger, it->second);
        }
      }
      logger->up();
    }

    //--------------------------------------------------------------------------
    void RegionNode::print_logical_state(CurrentState &state,
                                         const FieldMask &capture_mask,
                         LegionMap<ColorPoint,FieldMask>::aligned &to_traverse,
                                         TreeStateLogger *logger)
    //--------------------------------------------------------------------------
    {
      // Open Field States 
      {
        logger->log("Open Field States (%ld)", state.field_states.size());
        logger->down();
        for (std::list<FieldState>::const_iterator it = 
              state.field_states.begin(); it != 
              state.field_states.end(); it++)
        {
          it->print_state(logger, capture_mask);
          if (it->valid_fields * capture_mask)
            continue;
          for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator 
                cit = it->open_children.begin(); cit != 
                it->open_children.end(); cit++)
          {
            FieldMask overlap = cit->second & capture_mask;
            if (!overlap)
              continue;
            if (to_traverse.find(cit->first) == to_traverse.end())
              to_traverse[cit->first] = overlap;
            else
              to_traverse[cit->first] |= overlap;
          }
        }
        logger->up();
      }
    }
    
#ifdef DEBUG_LEGION
    //--------------------------------------------------------------------------
    void RegionNode::dump_logical_context(ContextID ctx, 
                                          TreeStateLogger *logger,
                                          const FieldMask &capture_mask)
    //--------------------------------------------------------------------------
    {
      switch (row_source->color.get_dim())
      {
        case 0:
          {
            logger->log("Region Node (%x,%d,%d) Color %d at "
                        "depth %d (%p)", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color.get_index(), logger->get_depth(), this);
            break;
          }
        case 1:
          {
            logger->log("Region Node (%x,%d,%d) Color %d at "
                        "depth %d (%p)", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], logger->get_depth(), this);
            break;
          }
        case 2:
          {
            logger->log("Region Node (%x,%d,%d) Color (%d,%d) at "
                        "depth %d (%p)", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], 
              row_source->color[1], logger->get_depth(), this);
            break;
          }
        case 3:
          {
            logger->log("Region Node (%x,%d,%d) Color (%d,%d,%d) at "
                        "depth %d (%p)", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->color[2],
              row_source->color[2], logger->get_depth(), this);
            break;
          }
        default:
          assert(false);
      }
      logger->down();
      LegionMap<ColorPoint,FieldMask>::aligned to_traverse;
      if (current_states.has_entry(ctx))
        print_logical_state(get_current_state(ctx), capture_mask,
                            to_traverse, logger);
      else
        logger->log("No state");
      logger->log("");
      if (!to_traverse.empty())
      {
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it =  
              to_traverse.begin(); it != to_traverse.end(); it++)
        {
          std::map<ColorPoint,PartitionNode*>::const_iterator finder = 
            color_map.find(it->first);
          if (finder != color_map.end())
            finder->second->dump_logical_context(ctx, logger, it->second);
        }
      }
      logger->up();
    }

    //--------------------------------------------------------------------------
    void RegionNode::dump_physical_context(ContextID ctx,
                                           TreeStateLogger *logger,
                                           const FieldMask &capture_mask)
    //--------------------------------------------------------------------------
    {
      switch (row_source->color.get_dim())
      {
        case 0:
          {
            logger->log("Region Node (%x,%d,%d) Color %d at "
                        "depth %d (%p)", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color.get_index(), logger->get_depth(), this);
            break;
          }
        case 1:
          {
            logger->log("Region Node (%x,%d,%d) Color %d at "
                        "depth %d (%p)", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], logger->get_depth(), this);
            break;
          }
        case 2:
          {
            logger->log("Region Node (%x,%d,%d) Color (%d,%d) at "
                        "depth %d (%p)", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], 
              row_source->color[1], logger->get_depth(), this);
            break;
          }
        case 3:
          {
            logger->log("Region Node (%x,%d,%d) Color (%d,%d,%d) at "
                        "depth %d (%p)", 
              handle.index_space.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->color[2],
              row_source->color[2], logger->get_depth(), this);
            break;
          }
        default:
          assert(false);
      }
      logger->down();
      LegionMap<ColorPoint,FieldMask>::aligned to_traverse;
      if (current_states.has_entry(ctx))
      {
        CurrentState &state = get_current_state(ctx);
        state.print_physical_state(this, capture_mask, to_traverse, logger);
      }
      else
        logger->log("No state");
      logger->log("");
      if (!to_traverse.empty())
      {
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it =
              to_traverse.begin(); it != to_traverse.end(); it++)
        {
          std::map<ColorPoint,PartitionNode*>::const_iterator finder = 
            color_map.find(it->first);
          if (finder != color_map.end())
            finder->second->dump_physical_context(ctx, logger, it->second);
        }
      }
      logger->up();
    }
#endif

    /////////////////////////////////////////////////////////////
    // Partition Node 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    PartitionNode::PartitionNode(LogicalPartition p, RegionNode *par,
                                 IndexPartNode *row_src, 
                                 FieldSpaceNode *col_src,
                                 RegionTreeForest *ctx)
      : RegionTreeNode(ctx, col_src), handle(p), 
        parent(par), row_source(row_src)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    PartitionNode::PartitionNode(const PartitionNode &rhs)
      : RegionTreeNode(NULL, NULL), handle(LogicalPartition::NO_PART),
        parent(NULL), row_source(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    PartitionNode::~PartitionNode(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    PartitionNode& PartitionNode::operator=(const PartitionNode &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void* PartitionNode::operator new(size_t count)
    //--------------------------------------------------------------------------
    {
      return legion_alloc_aligned<PartitionNode,true/*bytes*/>(count);
    }

    //--------------------------------------------------------------------------
    void PartitionNode::operator delete(void *ptr)
    //--------------------------------------------------------------------------
    {
      free(ptr);
    }

    //--------------------------------------------------------------------------
    bool PartitionNode::has_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock,1,false/*exclusive*/);
      return (color_map.find(c) != color_map.end());
    }

    //--------------------------------------------------------------------------
    bool PartitionNode::has_color(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      // Ask the row source because it eagerly instantiates
      return row_source->has_child(c);
    }

    //--------------------------------------------------------------------------
    RegionNode* PartitionNode::get_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      // check to see if we have it, if not try to make it
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        std::map<ColorPoint,RegionNode*>::const_iterator finder = 
          color_map.find(c);
        if (finder != color_map.end())
          return finder->second;
      }
      // If we get here we didn't immediately have it so try
      // to make it through the proper channels
      IndexSpaceNode *index_node = row_source->get_child(c);
#ifdef DEBUG_LEGION
      assert(index_node != NULL);
#endif
      LogicalRegion reg_handle(handle.tree_id, index_node->handle,
                               handle.field_space);
      return context->create_node(reg_handle, this);
    }

    //--------------------------------------------------------------------------
    void PartitionNode::add_child(RegionNode *child)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
#ifdef DEBUG_LEGION
      assert(color_map.find(child->row_source->color) == color_map.end());
#endif
      color_map[child->row_source->color] = child;
      valid_map[child->row_source->color] = child;
    }

    //--------------------------------------------------------------------------
    void PartitionNode::remove_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      valid_map.erase(c);
    }

    //--------------------------------------------------------------------------
    void PartitionNode::add_creation_source(AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      AutoLock n_lock(node_lock);
      creation_set.add(source);
    }

    //--------------------------------------------------------------------------
    void PartitionNode::DestructionFunctor::apply(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      runtime->send_logical_partition_destruction(handle, target);
    }

    //--------------------------------------------------------------------------
    void PartitionNode::destroy_node(AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      // If we've already been destroyed then we are done
      if (destroyed)
        return;
      AutoLock n_lock(node_lock);
      if (!destroyed)
      {
        destroyed = true;
        if (!creation_set.empty())
        {
          DestructionFunctor functor(handle, context->runtime);
          creation_set.map(functor);
        }
      }
    }

    //--------------------------------------------------------------------------
    unsigned PartitionNode::get_depth(void) const
    //--------------------------------------------------------------------------
    {
      return row_source->depth;
    }

    //--------------------------------------------------------------------------
    const ColorPoint& PartitionNode::get_color(void) const
    //--------------------------------------------------------------------------
    {
      return row_source->color;
    }

    //--------------------------------------------------------------------------
    IndexTreeNode* PartitionNode::get_row_source(void) const
    //--------------------------------------------------------------------------
    {
      return row_source;
    }

    //--------------------------------------------------------------------------
    RegionTreeID PartitionNode::get_tree_id(void) const
    //--------------------------------------------------------------------------
    {
      return handle.get_tree_id();
    }

    //--------------------------------------------------------------------------
    RegionTreeNode* PartitionNode::get_parent(void) const
    //--------------------------------------------------------------------------
    {
      return parent;
    }

    //--------------------------------------------------------------------------
    RegionTreeNode* PartitionNode::get_tree_child(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      return get_child(c);
    }

    //--------------------------------------------------------------------------
    ApEvent PartitionNode::issue_copy(Operation *op,
                        const std::vector<Domain::CopySrcDstField> &src_fields,
                        const std::vector<Domain::CopySrcDstField> &dst_fields,
                        ApEvent precondition,RegionTreeNode *intersect/*=NULL*/,
                        ReductionOpID redop /*=0*/,bool reduction_fold/*=true*/)
    //--------------------------------------------------------------------------
    {
      return parent->issue_copy(op, src_fields, dst_fields, precondition,
                                intersect, redop, reduction_fold);
    }

    //--------------------------------------------------------------------------
    ApEvent PartitionNode::issue_fill(Operation *op,
                        const std::vector<Domain::CopySrcDstField> &dst_fields,
                        const void *fill_value, size_t fill_size,
                        ApEvent precondition, RegionTreeNode *intersect)
    //--------------------------------------------------------------------------
    {
      return parent->issue_fill(op, dst_fields, fill_value, fill_size, 
                                precondition, intersect);
    }

    //--------------------------------------------------------------------------
    bool PartitionNode::are_children_disjoint(const ColorPoint &c1, 
                                              const ColorPoint &c2)
    //--------------------------------------------------------------------------
    {
      return row_source->are_disjoint(c1, c2);
    }

    //--------------------------------------------------------------------------
    bool PartitionNode::are_all_children_disjoint(void)
    //--------------------------------------------------------------------------
    {
      return row_source->is_disjoint();
    }

    //--------------------------------------------------------------------------
    void PartitionNode::instantiate_children(void)
    //--------------------------------------------------------------------------
    {
      std::set<ColorPoint> all_colors;
      row_source->get_colors(all_colors);
      // This may look like it does nothing, but it checks to see
      // if we have instantiated all the child nodes
      for (std::set<ColorPoint>::const_iterator it = all_colors.begin(); 
            it != all_colors.end(); it++)
        get_child(*it);
    }

    //--------------------------------------------------------------------------
    bool PartitionNode::is_region(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

#ifdef DEBUG_LEGION
    //--------------------------------------------------------------------------
    RegionNode* PartitionNode::as_region_node(void) const
    //--------------------------------------------------------------------------
    {
      return NULL;
    }

    //--------------------------------------------------------------------------
    PartitionNode* PartitionNode::as_partition_node(void) const
    //--------------------------------------------------------------------------
    {
      return const_cast<PartitionNode*>(this);
    }
#endif

    //--------------------------------------------------------------------------
    AddressSpaceID PartitionNode::get_owner_space(void) const
    //--------------------------------------------------------------------------
    {
      return get_owner_space(handle, context->runtime);
    }

    //--------------------------------------------------------------------------
    /*static*/ AddressSpaceID PartitionNode::get_owner_space(
                                     LogicalPartition handle, Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      return (handle.tree_id % runtime->runtime_stride);
    }

    //--------------------------------------------------------------------------
    bool PartitionNode::visit_node(PathTraverser *traverser)
    //--------------------------------------------------------------------------
    {
      return traverser->visit_partition(this);
    }

    //--------------------------------------------------------------------------
    bool PartitionNode::visit_node(NodeTraverser *traverser)
    //--------------------------------------------------------------------------
    {
      bool continue_traversal = traverser->visit_partition(this);
      if (continue_traversal)
      {
        const bool break_early = traverser->break_early();
        if (traverser->force_instantiation)
        {
          for (Domain::DomainPointIterator itr(row_source->color_space); 
                itr; itr++)
          {
            ColorPoint child_color(itr.p);
            bool result = get_child(child_color)->visit_node(traverser);
            continue_traversal = continue_traversal && result;
            if (!result && break_early)
              break;
          }
        }
        else
        {
          std::map<ColorPoint,RegionNode*> children;
          // Need to hold the lock when reading from 
          // the color map or the valid map
          if (traverser->visit_only_valid())
          {
            AutoLock n_lock(node_lock,1,false/*exclusive*/);
            children = valid_map;
          }
          else
          {
            AutoLock n_lock(node_lock,1,false/*exclusive*/);
            children = color_map;
          }
          for (std::map<ColorPoint,RegionNode*>::const_iterator it = 
                children.begin(); it != children.end(); it++)
          {
            bool result = it->second->visit_node(traverser);
            continue_traversal = continue_traversal && result;
            if (!result && break_early)
              break;
          }
        }
      }
      return continue_traversal;
    }

    //--------------------------------------------------------------------------
    const Domain& PartitionNode::get_domain_blocking(void) const
    //--------------------------------------------------------------------------
    {
 #ifdef DEBUG_LEGION
      assert(parent != NULL);
#endif     
      return parent->get_domain_blocking();
    }

    //--------------------------------------------------------------------------
    const Domain& PartitionNode::get_domain(ApEvent &precondition) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(parent != NULL);
#endif
      return parent->get_domain(precondition);
    }

    //--------------------------------------------------------------------------
    const Domain& PartitionNode::get_domain_no_wait(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(parent != NULL);
#endif
      return parent->get_domain_no_wait();
    }

    //--------------------------------------------------------------------------
    bool PartitionNode::is_complete(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(parent != NULL);
#endif
      return row_source->is_complete();
    }

    //--------------------------------------------------------------------------
    bool PartitionNode::intersects_with(RegionTreeNode *other)
    //--------------------------------------------------------------------------
    {
      if (other == this)
        return true;
      if (other->is_region())
        return row_source->intersects_with(
                    other->as_region_node()->row_source);
      else
        return row_source->intersects_with(
                    other->as_partition_node()->row_source);
    }

    //--------------------------------------------------------------------------
    bool PartitionNode::dominates(RegionTreeNode *other)
    //--------------------------------------------------------------------------
    {
      if (other == this)
        return true;
      if (other->is_region())
        return row_source->dominates(other->as_region_node()->row_source);
      else
        return row_source->dominates(other->as_partition_node()->row_source);
    }

    //--------------------------------------------------------------------------
    size_t PartitionNode::get_num_children(void) const
    //--------------------------------------------------------------------------
    {
      return row_source->get_num_children();
    }

    //--------------------------------------------------------------------------
    InterCloseOp* PartitionNode::create_close_op(Operation *creator,
                                                 const FieldMask &closing_mask,
                        const LegionMap<ColorPoint,FieldMask>::aligned &targets,
                                                 const VersionInfo &close_info,
                                                 const VersionInfo &ver_info,
                                                 const TraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
      InterCloseOp *op = context->runtime->get_available_inter_close_op(false);
      // Construct a region requirement for this operation
      // Make it a projection requirement so we walk to a partition
      RegionRequirement req(handle, 0/*projection id */,
                            READ_WRITE, EXCLUSIVE, trace_info.req.parent);
      // Compute the set of fields that we need
      column_source->get_field_set(closing_mask, 
                                   trace_info.req.privilege_fields,
                                   req.privilege_fields);
      // Now initialize the operation
      op->initialize(creator->get_parent(), req, targets,
                     trace_info.trace, trace_info.req_idx, 
                     close_info, ver_info, closing_mask, creator);
      return op;
    }

    //--------------------------------------------------------------------------
    ReadCloseOp* PartitionNode::create_read_only_close_op(Operation *creator,
                                            const FieldMask &closing_mask,
                        const LegionMap<ColorPoint,FieldMask>::aligned &targets,
                                            const TraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
      ReadCloseOp *op = context->runtime->get_available_read_close_op(false);
      // Construct a region requirement for this operation
      // Make it a projection requirement so we walk to a partition
      RegionRequirement req(handle, 0/*projection id */,
                            READ_WRITE, EXCLUSIVE, trace_info.req.parent);
      // Compute the set of fields that we need
      column_source->get_field_set(closing_mask, 
                                   trace_info.req.privilege_fields,
                                   req.privilege_fields);
      // Now initialize the operation
      op->initialize(creator->get_parent(), req, targets, trace_info.trace,
                     trace_info.req_idx, closing_mask, creator);
      return op;
    }

    //--------------------------------------------------------------------------
    void PartitionNode::send_node(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      // Check to see if we have it in our creation set in which
      // case we are done otherwise keep going up
      bool continue_up = false;
      bool send_deletion = false;
      {
        AutoLock n_lock(node_lock); 
        if (!creation_set.contains(target))
        {
          continue_up = true;
          creation_set.add(target);
        }
        if (destroyed)
          send_deletion = true;
      }
      if (continue_up)
      {
#ifdef DEBUG_LEGION
        assert(parent != NULL);
#endif
        // Send the parent node first
        parent->send_node(target);
        AutoLock n_lock(node_lock);
        for (LegionMap<SemanticTag,SemanticInfo>::aligned::iterator it = 
              semantic_info.begin(); it != semantic_info.end(); it++)
        {
          Serializer rez;
          {
            RezCheck z(rez);
            rez.serialize(handle);
            rez.serialize(it->first);
            rez.serialize(it->second.size);
            rez.serialize(it->second.buffer, it->second.size);
            rez.serialize(it->second.is_mutable);
          }
          context->runtime->send_logical_partition_semantic_info(target, rez);
        }
      }
      if (send_deletion)
        context->runtime->send_logical_partition_destruction(handle, target);
    }

    //--------------------------------------------------------------------------
    InstanceView* PartitionNode::find_context_view(PhysicalManager *manager,
                                                   SingleTask *context)
    //--------------------------------------------------------------------------
    {
      return convert_reference_partition(manager, context);
    }

    //--------------------------------------------------------------------------
    InstanceView* PartitionNode::convert_reference_partition(
                                  PhysicalManager *manager, SingleTask *context)
    //--------------------------------------------------------------------------
    {
      InstanceView *result = find_instance_view(manager, context);
      if (result != NULL)
        return result;
      // No need to bother with the check here to see if we've arrived
#ifdef DEBUG_LEGION
      assert(parent != NULL);
#endif
      InstanceView *parent_view = 
        parent->convert_reference_region(manager, context);
      result = parent_view->get_instance_subview(row_source->color);
      return result;
    }

    //--------------------------------------------------------------------------
    CompositeView* PartitionNode::convert_composite_view_partition(
                                                      CompositeView *view) const
    //--------------------------------------------------------------------------
    {
      // No need to bother with the check here
#ifdef DEBUG_LEGION
      assert(parent != NULL);
#endif
      CompositeView *parent_view = parent->convert_composite_view_region(view);
      return parent_view->get_subview(row_source->color)->
                             as_deferred_view()->as_composite_view();
    }

    //--------------------------------------------------------------------------
    void PartitionNode::convert_references_partition(
                                const std::vector<PhysicalManager*> &managers,
                                std::vector<bool> &up_mask, SingleTask *context,
                                std::vector<InstanceView*> &results)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(parent != NULL);
      assert(managers.size() == up_mask.size());
#endif
      // See which ones are still good here
      std::vector<unsigned> up_indexes;
      for (unsigned idx = 0; idx < managers.size(); idx++)
      {
        if (!up_mask[idx])
          continue;
        PhysicalManager *manager = managers[idx];
        results[idx] = find_instance_view(manager, context);
        if (results[idx] != NULL)
        {
          up_mask[idx] = false;
          continue;
        }
        up_indexes.push_back(idx);
      }
      if (!up_indexes.empty())
      {
        parent->convert_references_region(managers, up_mask, context, results);
        for (unsigned idx = 0; idx < up_indexes.size(); idx++)
        {
          unsigned index = up_indexes[idx];
          InstanceView *parent_view = results[index];
          results[index] = parent_view->get_instance_subview(row_source->color);
        }
      }
    }

    //--------------------------------------------------------------------------
    void PartitionNode::send_semantic_request(AddressSpaceID target,
             SemanticTag tag, bool can_fail, bool wait_until, RtUserEvent ready)
    //--------------------------------------------------------------------------
    {
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(tag);
        rez.serialize(can_fail);
        rez.serialize(wait_until);
        rez.serialize(ready);
      }
      context->runtime->send_logical_partition_semantic_request(target, rez);
    }

    //--------------------------------------------------------------------------
    void PartitionNode::send_semantic_info(AddressSpaceID target,
                                           SemanticTag tag,
                                           const void *buffer, size_t size,
                                           bool is_mutable)
    //--------------------------------------------------------------------------
    {
      // Package up the message first
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(handle);
        rez.serialize(tag);
        rez.serialize(size);
        rez.serialize(buffer, size);
        rez.serialize(is_mutable);
      }
      context->runtime->send_logical_partition_semantic_info(target, rez);
    }

    //--------------------------------------------------------------------------
    void PartitionNode::process_semantic_request(SemanticTag tag,
       AddressSpaceID source, bool can_fail, bool wait_until, RtUserEvent ready)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(get_owner_space() == context->runtime->address_space);
#endif
      RtEvent precondition;
      void *result = NULL;
      size_t size = 0;
      bool is_mutable = false;
      {
        AutoLock n_lock(node_lock);
        // See if we already have the data
        LegionMap<SemanticTag,SemanticInfo>::aligned::iterator finder = 
          semantic_info.find(tag);
        if (finder != semantic_info.end())
        {
          if (finder->second.is_valid())
          {
            result = finder->second.buffer;
            size = finder->second.size;
            is_mutable = finder->second.is_mutable;
          }
          else if (!can_fail && wait_until)
            precondition = finder->second.ready_event;
        }
        else if (!can_fail && wait_until)
        {
          // Don't have it yet, make a condition and hope that one comes
          RtUserEvent ready_event = Runtime::create_rt_user_event();
          precondition = ready_event;
          semantic_info[tag] = SemanticInfo(ready_event);
        }
      }
      if (result == NULL)
      {
        if (can_fail || !wait_until)
          Runtime::trigger_event(ready);
        else
        {
          // Defer this until the semantic condition is ready
          SemanticRequestArgs args;
          args.hlr_id = HLR_PARTITION_SEMANTIC_INFO_REQ_TASK_ID;
          args.proxy_this = this;
          args.tag = tag;
          args.source = source;
          context->runtime->issue_runtime_meta_task(&args, sizeof(args),
                              HLR_PARTITION_SEMANTIC_INFO_REQ_TASK_ID,
                              HLR_LATENCY_PRIORITY, NULL/*op*/, precondition);
        }
      }
      else
        send_semantic_info(source, tag, result, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    /*static*/ void PartitionNode::handle_semantic_request(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      LogicalPartition handle;
      derez.deserialize(handle);
      SemanticTag tag;
      derez.deserialize(tag);
      bool can_fail;
      derez.deserialize(can_fail);
      bool wait_until;
      derez.deserialize(wait_until);
      RtUserEvent ready;
      derez.deserialize(ready);
      PartitionNode *node = forest->get_node(handle);
      node->process_semantic_request(tag, source, can_fail, wait_until, ready);
    }

    //--------------------------------------------------------------------------
    /*static*/ void PartitionNode::handle_semantic_info(
           RegionTreeForest *forest, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      LogicalPartition handle;
      derez.deserialize(handle);
      SemanticTag tag;
      derez.deserialize(tag);
      size_t size;
      derez.deserialize(size);
      const void *buffer = derez.get_current_pointer();
      derez.advance_pointer(size);
      bool is_mutable;
      derez.deserialize(is_mutable);
      forest->attach_semantic_information(handle, tag, source, 
                                          buffer, size, is_mutable);
    }

    //--------------------------------------------------------------------------
    void PartitionNode::print_logical_context(ContextID ctx,
                                              TreeStateLogger *logger,
                                              const FieldMask &capture_mask)
    //--------------------------------------------------------------------------
    {
      switch (row_source->color.get_dim())
      {
        case 0:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color %d "
                        "disjoint at depth %d", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color.get_index(), row_source->is_disjoint(), 
              logger->get_depth());
            break;
          }
        case 1:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color %d "
                        "disjoint %d at depth %d", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->is_disjoint(), 
              logger->get_depth());
            break;
          }
        case 2:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color (%d,%d) "
                        "disjoint %d at depth %d", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], 
              row_source->color[1], 
              row_source->is_disjoint(), logger->get_depth());
            break;
          }
        case 3:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color (%d,%d,%d) "
                        "disjoint at depth %d", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->color[2],
              row_source->color[2], 
              row_source->is_disjoint(), logger->get_depth());
            break;
          }
        default:
          assert(false);
      }
      logger->down();
      LegionMap<ColorPoint,FieldMask>::aligned to_traverse;
      if (current_states.has_entry(ctx))
      {
        CurrentState &state = get_current_state(ctx);
        print_logical_state(state, capture_mask, to_traverse, logger);    
      }
      else
      {
        logger->log("No state");
      }
      logger->log("");
      if (!to_traverse.empty())
      {
        AutoLock n_lock(node_lock,1,false/*exclusive*/);
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it =
              to_traverse.begin(); it != to_traverse.end(); it++)
        {
          std::map<ColorPoint,RegionNode*>::const_iterator finder = 
            color_map.find(it->first);
          if (finder != color_map.end())
            finder->second->print_logical_context(ctx, logger, it->second);
        }
      }
      logger->up();
    }

    //--------------------------------------------------------------------------
    void PartitionNode::print_physical_context(ContextID ctx,
                                               TreeStateLogger *logger,
                                               const FieldMask &capture_mask)
    //--------------------------------------------------------------------------
    {
      switch (row_source->color.get_dim())
      {
        case 0:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color %d "
                        "disjoint at depth %d", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color.get_index(), 
              row_source->is_disjoint(), logger->get_depth());
            break;
          }
        case 1:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color %d "
                        "disjoint %d at depth %d", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], 
              row_source->is_disjoint(), logger->get_depth());
            break;
          }
        case 2:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color (%d,%d) "
                        "disjoint %d at depth %d", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], 
              row_source->color[1], 
              row_source->is_disjoint(), logger->get_depth());
            break;
          }
        case 3:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color (%d,%d,%d) "
                        "disjoint at depth %d", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->color[2],
              row_source->color[2], 
              row_source->is_disjoint(), logger->get_depth());
            break;
          }
        default:
          assert(false);
      }
      logger->down();
      LegionMap<ColorPoint,FieldMask>::aligned to_traverse;
      if (current_states.has_entry(ctx))
      {
        CurrentState &state = get_current_state(ctx);
        state.print_physical_state(this, capture_mask, to_traverse, logger);
      }
      else
      {
        logger->log("No state");
      }
      logger->log("");
      if (!to_traverse.empty())
      {
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it =
              to_traverse.begin(); it != to_traverse.end(); it++)
        {
          std::map<ColorPoint,RegionNode*>::const_iterator 
            finder = color_map.find(it->first);
          if (finder != color_map.end())
            finder->second->print_physical_context(ctx, logger, it->second);
        }
      }
      logger->up();
    }

    //--------------------------------------------------------------------------
    void PartitionNode::print_logical_state(CurrentState &state,
                                        const FieldMask &capture_mask,
                   LegionMap<ColorPoint,FieldMask>::aligned &to_traverse,
                                        TreeStateLogger *logger)
    //--------------------------------------------------------------------------
    {
      // Open Field States
      {
        logger->log("Open Field States (%ld)", state.field_states.size()); 
        logger->down();
        for (std::list<FieldState>::const_iterator it = 
              state.field_states.begin(); it != 
              state.field_states.end(); it++)
        {
          it->print_state(logger, capture_mask);
          if (it->valid_fields * capture_mask)
            continue;
          for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator 
                cit = it->open_children.begin(); cit != 
                it->open_children.end(); cit++)
          {
            FieldMask overlap = cit->second & capture_mask;
            if (!overlap)
              continue;
            if (to_traverse.find(cit->first) == to_traverse.end())
              to_traverse[cit->first] = overlap;
            else
              to_traverse[cit->first] |= overlap;
          }
        }
        logger->up();
      }
    }

#ifdef DEBUG_LEGION
    //--------------------------------------------------------------------------
    void PartitionNode::dump_logical_context(ContextID ctx,
                                             TreeStateLogger *logger,
                                             const FieldMask &capture_mask)
    //--------------------------------------------------------------------------
    {
      switch (row_source->color.get_dim())
      {
        case 0:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color %d "
                        "disjoint at depth %d (%p)", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color.get_index(), row_source->is_disjoint(), 
              logger->get_depth(), this);
            break;
          }
        case 1:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color %d "
                        "disjoint %d at depth %d (%p)", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->is_disjoint(), 
              logger->get_depth(), this);
            break;
          }
        case 2:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color (%d,%d) "
                        "disjoint %d at depth %d (%p)", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], 
              row_source->color[1], row_source->is_disjoint(), 
              logger->get_depth(), this);
            break;
          }
        case 3:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color (%d,%d,%d) "
                        "disjoint at depth %d (%p)", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->color[2],
              row_source->color[2], row_source->is_disjoint(), 
              logger->get_depth(), this);
            break;
          }
        default:
          assert(false);
      }
      logger->down();
      LegionMap<ColorPoint,FieldMask>::aligned to_traverse;
      if (current_states.has_entry(ctx))
      {
        CurrentState &state = get_current_state(ctx);
        print_logical_state(state, capture_mask, to_traverse, logger);
      }
      else
      {
        logger->log("No state");
      }
      logger->log("");
      if (!to_traverse.empty())
      {
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it =
              to_traverse.begin(); it != to_traverse.end(); it++)
        {
          std::map<ColorPoint,RegionNode*>::const_iterator finder = 
            color_map.find(it->first);
          if (finder != color_map.end())
            finder->second->dump_logical_context(ctx, logger, it->second);
        }
      }
      logger->up();
    }

    //--------------------------------------------------------------------------
    void PartitionNode::dump_physical_context(ContextID ctx,
                                              TreeStateLogger *logger,
                                              const FieldMask &capture_mask)
    //--------------------------------------------------------------------------
    {
      switch (row_source->color.get_dim())
      {
        case 0:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color %d "
                        "disjoint at depth %d (%p)", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color.get_index(), row_source->is_disjoint(), 
              logger->get_depth(), this);
            break;
          }
        case 1:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color %d "
                        "disjoint %d at depth %d (%p)", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->is_disjoint(), 
              logger->get_depth(), this);
            break;
          }
        case 2:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color (%d,%d) "
                        "disjoint %d at depth %d (%p)", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], 
              row_source->color[1], row_source->is_disjoint(), 
              logger->get_depth(), this);
            break;
          }
        case 3:
          {
            logger->log("Partition Node (" IDFMT ",%d,%d) Color (%d,%d,%d) "
                        "disjoint at depth %d (%p)", 
              handle.index_partition.id, handle.field_space.id,handle.tree_id,
              row_source->color[0], row_source->color[2],
              row_source->color[2], row_source->is_disjoint(), 
              logger->get_depth(), this);
            break;
          }
        default:
          assert(false);
      }
      logger->down();
      LegionMap<ColorPoint,FieldMask>::aligned to_traverse;
      if (current_states.has_entry(ctx))
      {
        CurrentState &state = get_current_state(ctx);
        state.print_physical_state(this, capture_mask, to_traverse, logger);
      }
      else
      {
        logger->log("No state");
      }
      logger->log("");
      if (!to_traverse.empty())
      {
        for (LegionMap<ColorPoint,FieldMask>::aligned::const_iterator it =
              to_traverse.begin(); it != to_traverse.end(); it++)
        {
          std::map<ColorPoint,RegionNode*>::const_iterator finder = 
            color_map.find(it->first);
          if (finder != color_map.end())
            finder->second->dump_physical_context(ctx, logger, it->second);
        }
      }
      logger->up();
    }
#endif 

  }; // namespace Internal 
}; // namespace Legion

// EOF

