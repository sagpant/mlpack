/** @file distributed_table.h
 *
 *  @author Dongryeol Lee (dongryel@cc.gatech.edu)
 */

#ifndef CORE_TABLE_DISTRIBUTED_TABLE_H
#define CORE_TABLE_DISTRIBUTED_TABLE_H

#include <boost/mpi.hpp>
#include <boost/mpi/timer.hpp>
#include <boost/mpi/collectives.hpp>
#include <boost/serialization/string.hpp>
#include <boost/interprocess/offset_ptr.hpp>
#include <boost/random/variate_generator.hpp>
#include <boost/utility.hpp>
#include "core/table/table.h"
#include "core/table/memory_mapped_file.h"
#include "core/tree/gen_metric_tree.h"
#include "core/table/distributed_auction.h"
#include "core/table/offset_dense_matrix.h"
#include "core/tree/distributed_local_kmeans.h"
#include "core/table/index_util.h"

namespace core {
namespace table {

extern MemoryMappedFile *global_m_file_;

template<typename TreeSpecType>
class DistributedTable: public boost::noncopyable {

  public:

    typedef core::tree::GeneralBinarySpaceTree <TreeSpecType> TreeType;

    typedef core::table::Table <
    TreeSpecType, std::pair<int, std::pair< int, int> > > TableType;

    typedef DistributedTable<TreeSpecType> DistributedTableType;

    typedef std::pair<int, int> IndexType;

  public:
    class TreeIterator {
      private:
        int begin_;

        int end_;

        int current_index_;

        const DistributedTableType *table_;

      public:

        TreeIterator() {
          begin_ = -1;
          end_ = -1;
          current_index_ = -1;
          table_ = NULL;
        }

        TreeIterator(const TreeIterator &it_in) {
          begin_ = it_in.begin();
          end_ = it_in.end();
          current_index_ = it_in.current_index();
          table_ = it_in.table();
        }

        TreeIterator(const DistributedTableType &table, const TreeType *node) {
          table_ = &table;
          begin_ = node->begin();
          end_ = node->end();
          current_index_ = begin_ - 1;
        }

        TreeIterator(const DistributedTableType &table, int begin, int count) {
          table_ = &table;
          begin_ = begin;
          end_ = begin + count;
          current_index_ = begin_ - 1;
        }

        const DistributedTableType *table() const {
          return table_;
        }

        bool HasNext() const {
          return current_index_ < end_ - 1;
        }

        void Next() {
          current_index_++;
        }

        void Next(core::table::DensePoint *entry, int *point_id) {
          current_index_++;
          table_->iterator_get_(current_index_, entry);
          *point_id = table_->iterator_get_id_(current_index_);
        }

        void get(int i, core::table::DensePoint *entry) {
          table_->iterator_get_(begin_ + i, entry);
        }

        void get_id(int i, int *point_id) {
          *point_id = table_->iterator_get_id_(begin_ + i);
        }

        void RandomPick(core::table::DensePoint *entry) {
          table_->iterator_get_(core::math::Random(begin_, end_), entry);
        }

        void RandomPick(core::table::DensePoint *entry, int *point_id) {
          *point_id = core::math::Random(begin_, end_);
          table_->iterator_get_(*point_id, entry);
        }

        void Reset() {
          current_index_ = begin_ - 1;
        }

        int current_index() const {
          return current_index_;
        }

        int count() const {
          return end_ - begin_;
        }

        int begin() const {
          return begin_;
        }

        int end() const {
          return end_;
        }
    };

  private:

    boost::interprocess::offset_ptr<TableType> owned_table_;

    boost::interprocess::offset_ptr<int> local_n_entries_;

    boost::interprocess::offset_ptr<TableType> global_table_;

    int table_outbox_group_comm_size_;

  private:

    void ReplenishNodes_(std::vector<TreeType *> &top_leaf_nodes) {

      core::table::DensePoint tmp_point;
      arma::vec tmp_point_alias;
      tmp_point.Init(top_leaf_nodes[0]->bound().center().length());
      core::table::DensePointToArmaVec(tmp_point, &tmp_point_alias);
      int num_additional = table_outbox_group_comm_size_ -
                           top_leaf_nodes.size();
      int num_samples = std::max(1, core::math::RandInt(top_leaf_nodes.size()));

      // Randomly add new dummy nodes with randomly chosen centroids
      // averaged.
      for(int j = 0; j < num_additional; j++) {
        tmp_point_alias.zeros();
        for(int i = 0; i < num_samples; i++) {
          arma::vec random_node_center;
          core::table::DensePointToArmaVec(
            top_leaf_nodes[
              core::math::RandInt(top_leaf_nodes.size())]->bound().center(),
            &random_node_center);
          tmp_point_alias += random_node_center;
        }
        tmp_point_alias = (1.0 / static_cast<double>(num_samples)) *
                          tmp_point_alias;
        top_leaf_nodes.push_back(new TreeType());
        top_leaf_nodes[ top_leaf_nodes.size() - 1 ]->bound().center().Copy(
          tmp_point);
      }
    }

    void ReadjustCentroids_(
      boost::mpi::communicator &table_outbox_group_comm,
      const core::metric_kernels::AbstractMetric &metric,
      const std::vector<TreeType *> &top_leaf_nodes,
      int leaf_node_assignment_index) {

      const int neighbor_radius = table_outbox_group_comm.size();
      const int num_iterations = 10;

      // Readjust the centroid.
      std::vector<int> point_assignments;
      int total_num_points_owned;
      core::tree::DistributedLocalKMeans local_kmeans;
      local_kmeans.Compute(
        table_outbox_group_comm, metric, *owned_table_,
        neighbor_radius, num_iterations,
        top_leaf_nodes[leaf_node_assignment_index]->bound().center(),
        &total_num_points_owned, &point_assignments);

      // Move the data across processes to get a new local table.
      TableType *new_local_table =
        (core::table::global_m_file_) ?
        core::table::global_m_file_->Construct<TableType>() : new TableType();
      new_local_table->Init(
        owned_table_->n_attributes(), total_num_points_owned);

      // Left contributions.
      std::vector < core::table::OffsetDenseMatrix > left_contributions;
      std::vector < core::table::OffsetDenseMatrix > right_contributions;
      std::vector< boost::mpi::request > left_send_requests;
      std::vector< boost::mpi::request > right_send_requests;
      left_contributions.resize(
        std::min(table_outbox_group_comm.rank(), neighbor_radius));
      left_send_requests.resize(left_contributions.size());
      right_contributions.resize(
        std::min(
          table_outbox_group_comm.size() -
          table_outbox_group_comm.rank() - 1, neighbor_radius));
      right_send_requests.resize(right_contributions.size());
      for(unsigned int i = 1; i <= left_contributions.size(); i++) {
        left_contributions[i - 1].Init(
          table_outbox_group_comm.rank(),
          owned_table_->data(), owned_table_->old_from_new(), point_assignments,
          table_outbox_group_comm.rank() - i);
        left_send_requests[i - 1] =
          table_outbox_group_comm.isend(
            table_outbox_group_comm.rank() - i, i, left_contributions[i - 1]);
      }
      for(unsigned int i = 1; i <= right_contributions.size(); i++) {
        right_contributions[i - 1].Init(
          table_outbox_group_comm.rank(),
          owned_table_->data(), owned_table_->old_from_new(), point_assignments,
          table_outbox_group_comm.rank() + i);
        right_send_requests[i - 1] =
          table_outbox_group_comm.isend(
            table_outbox_group_comm.rank() + i, neighbor_radius + i,
            right_contributions[i - 1]);
      }

      // Receive the points needed by this process from other
      // processes. For the points owned by the process itself, just
      // copy over.
      core::table::OffsetDenseMatrix tmp_offset;
      double *new_table_ptr = new_local_table->data().ptr();
      std::pair<int, std::pair<int, int> > *new_table_old_from_new_ptr =
        new_local_table->old_from_new();
      for(unsigned int i = 1; i <= left_contributions.size(); i++) {
        tmp_offset.Init(
          table_outbox_group_comm.rank(),
          new_table_ptr, new_table_old_from_new_ptr,
          new_local_table->n_attributes());
        table_outbox_group_comm.recv(
          table_outbox_group_comm.rank() - i, neighbor_radius + i, tmp_offset);

        // Increment the table pointer based on the number of doubles
        // received.
        new_table_ptr += tmp_offset.n_entries() * tmp_offset.n_attributes();
        new_table_old_from_new_ptr += tmp_offset.n_entries();
      }
      tmp_offset.Init(
        table_outbox_group_comm.rank(),
        owned_table_->data(), new_table_old_from_new_ptr, point_assignments,
        table_outbox_group_comm.rank());
      tmp_offset.Extract(new_table_ptr, new_table_old_from_new_ptr);
      new_table_ptr += tmp_offset.n_entries() * tmp_offset.n_attributes();
      new_table_old_from_new_ptr += tmp_offset.n_entries();
      for(unsigned int i = 1; i <= right_contributions.size(); i++) {
        tmp_offset.Init(
          table_outbox_group_comm.rank(),
          new_table_ptr, new_table_old_from_new_ptr,
          new_local_table->n_attributes());
        table_outbox_group_comm.recv(
          table_outbox_group_comm.rank() + i, i, tmp_offset);

        // Increment the table pointer based on the number of doubles
        // received.
        new_table_ptr += tmp_offset.n_entries() * tmp_offset.n_attributes();
        new_table_old_from_new_ptr += tmp_offset.n_entries();
      }

      // Wait for all send/receive requests to be completed.
      boost::mpi::wait_all(
        left_send_requests.begin(), left_send_requests.end());
      boost::mpi::wait_all(
        right_send_requests.begin(), right_send_requests.end());
      table_outbox_group_comm.barrier();

      // Destory the old table and take the new table to be the owned
      // table.
      if(core::table::global_m_file_) {
        core::table::global_m_file_->DestroyPtr(owned_table_.get());
      }
      else {
        delete owned_table_.get();
      }
      owned_table_ = new_local_table;
    }

    int TakeLeafNodeOwnerShip_(
      boost::mpi::communicator &table_outbox_group_comm,
      const std::vector<double> &num_points_assigned_to_leaf_nodes) {

      if(table_outbox_group_comm.size() > 1) {
        core::table::DistributedAuction auction;
        return auction.Assign(
                 table_outbox_group_comm, num_points_assigned_to_leaf_nodes,
                 1.0 / static_cast<double>(table_outbox_group_comm.size()));
      }
      else {
        return 0;
      }
    }

    void GetLeafNodeMembershipCounts_(
      const core::metric_kernels::AbstractMetric &metric_in,
      const std::vector<TreeType *> &top_leaf_nodes,
      std::vector<double> &points_assigned_to_node) {

      for(unsigned int i = 0; i < top_leaf_nodes.size(); i++) {
        points_assigned_to_node[i] = 0;
      }

      // Loop through each point and find the closest leaf node.
      for(int i = 0; i < owned_table_->n_entries(); i++) {
        core::table::DensePoint point;
        owned_table_->get(i, &point);

        // Loop through each leaf node.
        double min_squared_mid_distance = std::numeric_limits<double>::max();
        int min_index = -1;
        for(unsigned int j = 0; j < top_leaf_nodes.size(); j++) {
          const typename TreeType::BoundType &leaf_node_bound =
            top_leaf_nodes[j]->bound();

          // Compute the squared mid-distance.
          double squared_mid_distance = leaf_node_bound.MidDistanceSq(
                                          metric_in, point);
          if(squared_mid_distance < min_squared_mid_distance) {
            min_squared_mid_distance = squared_mid_distance;
            min_index = j;
          }
        }

        // Output the assignments.
        points_assigned_to_node[min_index] += 1.0;
      }
    }

    void SelectSubset_(
      double sample_probability_in, std::vector<int> *sampled_indices_out) {

      std::vector<int> indices(owned_table_->n_entries(), 0);
      for(unsigned int i = 0; i < indices.size(); i++) {
        indices[i] = i;
      }
      int num_elements =
        std::max(
          (int) floor(sample_probability_in * owned_table_->n_entries()), 1);
      for(int i = 0; i < num_elements; i++) {
        int random_index = core::math::RandInt(i, (int) indices.size());
        std::swap(indices[i], indices[ random_index ]);
      }

      for(int i = 0; i < num_elements; i++) {
        sampled_indices_out->push_back(indices[i]);
      }
    }

    void CopyPointsIntoTemporaryBuffer_(
      const std::vector<int> &sampled_indices, double **tmp_buffer) {

      *tmp_buffer = new double[ sampled_indices.size() * this->n_attributes()];

      for(unsigned int i = 0; i < sampled_indices.size(); i++) {
        owned_table_->get(
          sampled_indices[i], (*tmp_buffer) + i * this->n_attributes());
      }
    }

  public:

    DistributedTable() {
      owned_table_ = NULL;
      local_n_entries_ = NULL;
      global_table_ = NULL;
      table_outbox_group_comm_size_ = -1;
    }

    ~DistributedTable() {

      // Delete the list of number of entries for each table in the
      // distributed table.
      if(local_n_entries_.get() != NULL) {
        if(core::table::global_m_file_) {
          core::table::global_m_file_->DestroyPtr(local_n_entries_.get());
        }
        else {
          delete[] local_n_entries_.get();
        }
        local_n_entries_ = NULL;
      }

      // Delete the table.
      if(owned_table_.get() != NULL) {
        if(core::table::global_m_file_) {
          core::table::global_m_file_->DestroyPtr(owned_table_.get());
        }
        else {
          delete owned_table_.get();
        }
        owned_table_ = NULL;
      }

      // Delete the tree.
      if(global_table_.get() != NULL) {
        if(core::table::global_m_file_) {
          core::table::global_m_file_->DestroyPtr(global_table_.get());
        }
        else {
          delete global_table_.get();
        }
        global_table_ = NULL;
      }
    }

    TableType *local_table() {
      return owned_table_.get();
    }

    TreeType *get_tree() {
      return global_table_->get_tree();
    }

    int n_attributes() const {
      return owned_table_->n_attributes();
    }

    int local_n_entries(int rank_in) const {
      if(rank_in >= table_outbox_group_comm_size_) {
        printf(
          "Invalid rank specified: %d. %d is the limit.\n",
          rank_in, table_outbox_group_comm_size_);
        return -1;
      }
      return local_n_entries_[rank_in];
    }

    int n_entries() const {
      return owned_table_->n_entries();
    }

    void Init(
      const std::string & file_name,
      boost::mpi::communicator &table_outbox_group_communicator_in) {

      boost::mpi::timer distributed_table_init_timer;

      // Initialize the table owned by the distributed table.
      owned_table_ = (core::table::global_m_file_) ?
                     core::table::global_m_file_->Construct<TableType>() :
                     new TableType();
      owned_table_->Init(file_name, table_outbox_group_communicator_in.rank());

      // Allocate the vector for storing the number of entries for all
      // the tables in the world, and do an all-gather operation to
      // find out all the sizes.
      table_outbox_group_comm_size_ = table_outbox_group_communicator_in.size();
      local_n_entries_ = (core::table::global_m_file_) ?
                         (int *) global_m_file_->ConstructArray<int>(
                           table_outbox_group_communicator_in.size()) :
                         new int[ table_outbox_group_communicator_in.size()];
      boost::mpi::all_gather(
        table_outbox_group_communicator_in, owned_table_->n_entries(),
        local_n_entries_.get());

      if(table_outbox_group_communicator_in.rank() == 0) {
        printf(
          "Took %g seconds to read in the distributed tables.\n",
          distributed_table_init_timer.elapsed());
      }
    }

    void Save(const std::string & file_name) const {

    }

    bool IsIndexed() const {
      return global_table_->get_tree() != NULL;
    }

    void IndexData(
      const core::metric_kernels::AbstractMetric & metric_in,
      boost::mpi::communicator &table_outbox_group_comm,
      int leaf_size, double sample_probability_in) {

      boost::mpi::timer distributed_table_index_timer;

      // Each process generates a random subset of the data points to
      // send to the master. This is a MPI gather operation.
      TableType sampled_table;
      std::vector<int> sampled_indices;
      SelectSubset_(sample_probability_in, &sampled_indices);

      // Send the number of points chosen in this process to the
      // master so that the master can allocate the appropriate amount
      // of space to receive all the points.
      int total_num_samples = 0;
      double *tmp_buffer = NULL;
      int *counts = new int[ table_outbox_group_comm.size()];
      int local_sampled_indices_size = static_cast<int>(
                                         sampled_indices.size());
      boost::mpi::gather(
        table_outbox_group_comm, local_sampled_indices_size, counts, 0);
      for(int i = 0; i < table_outbox_group_comm.size(); i++) {
        total_num_samples += counts[i];
      }

      // Each process copies the subset of points into the temporary
      // buffer.
      CopyPointsIntoTemporaryBuffer_(sampled_indices, &tmp_buffer);

      if(table_outbox_group_comm.rank() == 0) {
        sampled_table.Init(this->n_attributes(), total_num_samples);
        memcpy(
          sampled_table.data().ptr(), tmp_buffer,
          sizeof(double) * this->n_attributes() * sampled_indices.size());

        int offset = this->n_attributes() * sampled_indices.size();
        for(int i = 1; i < table_outbox_group_comm.size(); i++) {
          int num_elements_to_receive = this->n_attributes() * counts[i];
          table_outbox_group_comm.recv(
            i, boost::mpi::any_tag,
            sampled_table.data().ptr() + offset, num_elements_to_receive);
          offset += num_elements_to_receive;
        }
      }
      else {
        table_outbox_group_comm.send(
          0, table_outbox_group_comm.rank(), tmp_buffer,
          this->n_attributes() * sampled_indices.size());
      }

      table_outbox_group_comm.barrier();

      // After sending, free the temporary buffer.
      delete[] tmp_buffer;
      delete[] counts;

      // The master builds the top tree, and sends the leaf nodes to
      // the rest.
      std::vector<TreeType *> top_leaf_nodes;
      if(table_outbox_group_comm.rank() == 0) {
        sampled_table.IndexData(
          metric_in, 1, table_outbox_group_comm.size());

        // Broadcast the leaf nodes.
        sampled_table.get_leaf_nodes(
          sampled_table.get_tree(), &top_leaf_nodes);
      }
      boost::mpi::broadcast(table_outbox_group_comm, top_leaf_nodes, 0);
      // Broadcast the leaf nodes.
      if(top_leaf_nodes.size() < static_cast<unsigned int>(
            table_outbox_group_comm.size())) {
        ReplenishNodes_(top_leaf_nodes);
      }

      // Assign each point to one of the leaf nodes.
      std::vector<double> num_points_assigned_to_leaf_nodes(
        top_leaf_nodes.size(), 0);
      GetLeafNodeMembershipCounts_(
        metric_in, top_leaf_nodes, num_points_assigned_to_leaf_nodes);

      // Each process takes a node in a greedy fashion to minimize the
      // data movement.
      int leaf_node_assignment_index = TakeLeafNodeOwnerShip_(
                                         table_outbox_group_comm,
                                         num_points_assigned_to_leaf_nodes);

      // Each process decides to test against the assigned leaf and
      // its immediate DFS neighbors.
      ReadjustCentroids_(
        table_outbox_group_comm, metric_in, top_leaf_nodes,
        leaf_node_assignment_index);

      // Index the local tree.
      owned_table_->IndexData(metric_in, leaf_size);

      // Now assemble the top tree. At this point, we can free the
      // leaf nodes for the non-master.
      if(table_outbox_group_comm.rank() != 0) {
        for(unsigned int i = 0; i < top_leaf_nodes.size(); i++) {
          delete top_leaf_nodes[i];
        }
      }

      // Every process gathers the adjusted leaf centroids and build
      // the top tree individually.
      global_table_ =
        (core::table::global_m_file_) ?
        core::table::global_m_file_->Construct<TableType>() :
        new TableType();
      global_table_->Init(
        owned_table_->n_attributes(), table_outbox_group_comm.size());
      boost::mpi::all_gather(
        table_outbox_group_comm,
        owned_table_->get_tree()->bound().center().ptr(),
        owned_table_->n_attributes(), global_table_->data().ptr());
      global_table_->IndexData(metric_in, 1);

      // Very important: need to re-update the counts.
      boost::mpi::all_gather(
        table_outbox_group_comm, owned_table_->n_entries(),
        local_n_entries_.get());

      if(table_outbox_group_comm.rank() == 0) {
        printf("Finished building the distributed tree.\n");
        printf(
          "Took %g seconds to read in the distributed tree.\n",
          distributed_table_index_timer.elapsed());
      }
    }

    TreeIterator get_node_iterator(TreeType *node) {
      return TreeIterator(*this, node);
    }

    TreeIterator get_node_iterator(int begin, int count) {
      return TreeIterator(*this, begin, count);
    }

  private:

    void direct_get_(int point_id, double *entry) const {
      if(this->IsIndexed() == false) {
        global_table_->data().MakeolumnVector(point_id, entry);
      }
      else {
        global_table_->data().MakeColumnVector(
          IndexUtil< IndexType>::Extract(
            global_table_->new_from_old(), point_id), entry);
      }
    }

    void direct_get_(
      int point_id, core::table::DensePoint *entry) const {
      if(this->IsIndexed() == false) {
        global_table_->data().MakeColumnVector(point_id, entry);
      }
      else {
        global_table_->data().MakeColumnVector(
          IndexUtil<IndexType>::Extract(
            global_table_->new_from_old(), point_id), entry);
      }
    }

    void direct_get_(int point_id, core::table::DensePoint *entry) {
      if(this->IsIndexed() == false) {
        global_table_->data().MakeColumnVector(point_id, entry);
      }
      else {
        global_table_->data().MakeColumnVector(
          IndexUtil<IndexType>::Extract(
            global_table_->new_from_old(), point_id), entry);
      }
    }

    void iterator_get_(
      int reordered_position, core::table::DensePoint *entry) const {
      global_table_->data().MakeColumnVector(reordered_position, entry);
    }

    void iterator_get_(
      int reordered_position, core::table::DensePoint *entry) {
      global_table_->data().MakeColumnVector(reordered_position, entry);
    }

    int iterator_get_id_(int reordered_position) const {
      if(this->IsIndexed() == false) {
        return reordered_position;
      }
      else {
        return IndexUtil<IndexType>::Extract(
                 global_table_->old_from_new(), reordered_position);
      }
    }
};
};
};

#endif
