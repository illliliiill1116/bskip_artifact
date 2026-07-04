// Copyright 2025 Yicong Luo and contributors

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>

#include "../include/BSkipList.hpp"

template <typename traits>
bool BSkip<traits>::insert(traits::element_type k)
{
    typename traits::key_type key = std::get<0>(k);

    // cannot insert sentinel
    if (key == traits::min_sentinel || key == traits::max_sentinel) {
        printf("cannot insert sentinel\n");
        return false;
    }

    int cpuid = sched_getcpu();
    ReaderWriterLock *parent_lock = nullptr;

    // flip coins to determine your promotion level
    uint32_t level_to_promote = flip_coins(key);
    BSkipNodeInternal<traits> *parent_node = NULL;

    // init all the new nodes you will need due to promotion split
    BSkipNode<traits>*  new_nodes[traits::MAX_KEYS];
    if (level_to_promote > 1)
    {
        for (uint32_t i = 0; i < level_to_promote - 1; i++)
        {
            auto n = new BSkipNodeInternal<traits>();
            if constexpr (traits::concurrent)
            {
                n->mutex_.write_lock();
            }
            new_nodes[i] = n;
        }
    }

    if (level_to_promote > 0)
    {
        auto n = new BSkipNodeLeaf<traits>();
        if constexpr (traits::concurrent)
        {
            n->mutex_.write_lock();
        }
        new_nodes[level_to_promote - 1] = n;
    }

    uint32_t num_split = 0;

    assert(level_to_promote < MAX_HEIGHT);

    auto curr_node = headers[MAX_HEIGHT - 1];

    for (uint level = MAX_HEIGHT; level-- > 0;)
    {
        if constexpr (traits::concurrent)
        {
            // if at an internal level, grab your current lock
            if (level > 0)
            {
                if (level_to_promote < level)
                {
                    ((BSkipNodeInternal<traits> *)(curr_node))->mutex_.read_lock(cpuid);
                }
                else
                {
                    ((BSkipNodeInternal<traits> *)(curr_node))->mutex_.write_lock();
                }
            }
            else
            {
                // if at a leaf, lock yourself
                ((BSkipNodeLeaf<traits> *)(curr_node))->mutex_.write_lock();
            }

            // if the previous level was higher than your promotion level, you had the
            // lock on the parent
            if (parent_lock)
            {
                if (level + 1 > level_to_promote)
                {
                    parent_lock->read_unlock(cpuid);
                    parent_lock = nullptr;
                }
                else
                {
                    assert(level_to_promote > level);
                    parent_lock->write_unlock();
                    parent_lock = nullptr;
                }
            }
        }
        tbassert(key >= curr_node->get_header(),
                 "level = %u, k = %lu, header = %lu\n", level, key,
                 curr_node->get_header());

        auto prev_node = curr_node;

        // find the node to insert the key in in this level
        while (curr_node->next_header <= key)
        {
            tbassert(curr_node->get_header() < curr_node->next->get_header(),
                     "curr node header %lu, next header %lu\n",
                     curr_node->get_header(), curr_node->next->get_header());
            tbassert(key >= curr_node->get_header(),
                     "key = %lu, curr node header = %lu, next node header = %lu\n", key,
                     curr_node->get_header(), curr_node->next->get_header());

            // grab next step in the search
            if constexpr (traits::concurrent)
            {
                if (level > 0)
                {
                    assert(curr_node->level > 0);
                    if (level_to_promote < level)
                    {
                        ((BSkipNodeInternal<traits> *)(curr_node->next))
                            ->mutex_.read_lock(cpuid);
                    }
                    else
                    {
                        ((BSkipNodeInternal<traits> *)(curr_node->next))
                            ->mutex_.write_lock();
                    }
                }
                else
                {
                    ((BSkipNodeLeaf<traits> *)(curr_node->next))->mutex_.write_lock();
                }
            }

            prev_node = curr_node;

            tbassert(curr_node->next->get_header() <= key,
                     "k = %lu, prev node = %lu, next node = %lu\n", key,
                     prev_node->get_header(), curr_node->next->get_header());

            curr_node = curr_node->next;

            // unlock prev node
            if constexpr (traits::concurrent)
            {
                if (level > 0)
                {
                    if (level_to_promote < level)
                    {
                        ((BSkipNodeInternal<traits> *)(prev_node))
                            ->mutex_.read_unlock(cpuid);
                    }
                    else
                    {
                        ((BSkipNodeInternal<traits> *)(prev_node))->mutex_.write_unlock();
                    }
                }
                else
                {
                    ((BSkipNodeLeaf<traits> *)(prev_node))->mutex_.write_unlock();
                }
            }
        }
        tbassert(curr_node->get_header() <= key,
                 "k = %lu, level to promote %u, level = %u, prev_node_header = "
                 "%lu, curr node = %lu\n",
                 key, level_to_promote, level, prev_node->get_header(),
                 curr_node->get_header());

        // now we are at the correct node - look for the key
        auto [rank, found_key] = curr_node->find_key_and_check(key);

        // if the key was found
        if (found_key)
        {

            // Fix: only release the lock here for the paths that don't
            // need to write anything else (sets, and internal levels for
            // maps, which re-lock lower down in the loop instead). The
            // map-at-level-0 path keeps holding the very same write lock
            // continuously through the value write, so no other thread can
            // observe or mutate the leaf in between.
            constexpr bool defers_unlock_to_map_leaf_write =
                !traits::binary;
            if constexpr (traits::concurrent)
            {
                // lock_timer.start();
                if (level > 0)
                {
                    if (level_to_promote < level)
                    {
                        ((BSkipNodeInternal<traits> *)(curr_node))
                            ->mutex_.read_unlock(cpuid);
                    }
                    else
                    {
                        ((BSkipNodeInternal<traits> *)(curr_node))->mutex_.write_unlock();
                    }
                }
                else if (!(defers_unlock_to_map_leaf_write && level == 0))
                {
                    ((BSkipNodeLeaf<traits> *)(curr_node))->mutex_.write_unlock();
                }
            }

            // check if map or set
            if constexpr (!traits::binary)
            {
                if (level == 0)
                {
                    // still holding the write lock acquired for the search
                    // above -- do NOT release and re-acquire it here, see
                    // the comment above.
                    /*
                    if constexpr (traits::concurrent)
                    {
                        ((BSkipNodeLeaf<traits> *)(curr_node))->mutex_.write_lock();
                    }
                    */

                    // change leaf value
                    assert(curr_node->level == 0);
                    ((BSkipNodeLeaf<traits> *)curr_node)->set_elt_at_rank(rank, k);
                    if constexpr (traits::concurrent)
                    {
                        ((BSkipNodeLeaf<traits> *)(curr_node))->mutex_.write_unlock();
                    }
                }
                else
                {
                    // curr_node is internal, need to traverse down to the leaf
                    if constexpr (traits::concurrent)
                    {
                        ((BSkipNodeInternal<traits> *)(curr_node))->mutex_.read_lock(cpuid);
                    }
                    bool flag = true;
                    while (curr_node->level > 1)
                    {
                        prev_node = curr_node;
                        if (flag)
                        {
                            assert(curr_node->level > 0);
                            curr_node = ((BSkipNodeInternal<traits> *)curr_node)->get_child_at_rank(rank);
                            flag = false;
                        }
                        else
                        {
                            assert(curr_node->level > 0);
                            curr_node = ((BSkipNodeInternal<traits> *)curr_node)->get_child_at_rank(0);
                        }
                        if constexpr (traits::concurrent)
                        {
                            // lock curr
                            assert(curr_node->level > 0);
                            ((BSkipNodeInternal<traits> *)curr_node)->mutex_.read_lock(cpuid);
                            // unlock prev
                            assert(prev_node->level > 0);
                            ((BSkipNodeInternal<traits> *)prev_node)->mutex_.read_unlock(cpuid);
                        }
                    }

                    // now level = 1, curr_node is internal and locked
                    assert(curr_node->level == 1);

                    prev_node = curr_node;

                    if (flag)
                    {
                        assert(curr_node->level > 0);
                        curr_node = ((BSkipNodeInternal<traits> *)curr_node)->get_child_at_rank(rank);
                    }
                    else
                    {
                        assert(curr_node->level > 0);
                        curr_node = ((BSkipNodeInternal<traits> *)curr_node)->get_child_at_rank(0);
                    }

                    if constexpr (traits::concurrent)
                    {
                        // lock curr
                        assert(curr_node->level == 0);
                        ((BSkipNodeLeaf<traits> *)curr_node)->mutex_.write_lock();
                        // unlock prev
                        assert(prev_node->level > 0);
                        ((BSkipNodeInternal<traits> *)prev_node)->mutex_.read_unlock(cpuid);
                    }

                    // change leaf value
                    assert(curr_node->level == 0);
                    ((BSkipNodeLeaf<traits> *)curr_node)->set_elt_at_rank(0, k);

                    if constexpr (traits::concurrent)
                    {
                        ((BSkipNodeLeaf<traits> *)(curr_node))->mutex_.write_unlock();
                    }
                }
            }

            return true;
        }
        else
        { // otherwise, this key was not found at this level
            assert(curr_node->get_key_at_rank(rank) < key);

            if (level_to_promote < level)
            {
                // case 1: do not promote to this level.
                // drop down a level (if you are here, you are at an internal node)
                assert(curr_node->level > 0);
                if constexpr (traits::concurrent)
                {
                    // lock_timer.start();
                    parent_lock = &(((BSkipNodeInternal<traits> *)curr_node)->mutex_);
                    parent_node = ((BSkipNodeInternal<traits> *)curr_node);
                }

                curr_node =
                    ((BSkipNodeInternal<traits> *)curr_node)->get_child_at_rank(rank);

                assert(curr_node != NULL);
                continue;
            }
            else if (level_to_promote == level)
            {
                // Case 2: insert but not split due to promotion
                // split if overfull
                if (curr_node->num_elts + 1 > traits::MAX_KEYS)
                {
                    BSkipNode<traits> *new_node;
                    if (level > 0)
                    {
                        new_node = new BSkipNodeInternal<traits>();
                    }
                    else
                    {
                        new_node = new BSkipNodeLeaf<traits>();
                    }

                    // grab lock on the new node
                    if constexpr (traits::concurrent)
                    {
                        // lock_timer.start();
                        if (level > 0)
                        {
                            ((BSkipNodeInternal<traits> *)(new_node))->mutex_.write_lock();
                        }
                        else
                        {
                            ((BSkipNodeLeaf<traits> *)(new_node))->mutex_.write_lock();
                        }
                    }

                    // fixup next pointers
                    new_node->next = curr_node->next;
                    new_node->next_header = curr_node->next_header;
                    curr_node->next = new_node;
                    new_node->level = level;

                    // do the split
                    int half_keys = curr_node->num_elts / 2;

                    // move second half of keys into new node
                    // returns the number of elements that were moved
                    // updates the number of elts in each node
                    uint32_t elts_moved = curr_node->split_keys(new_node, half_keys, 0);
                    curr_node->next_header = new_node->get_header();

                    // move children if necessary
                    if (level > 0)
                    {
                        ((BSkipNodeInternal<traits> *)curr_node)
                            ->move_children(((BSkipNodeInternal<traits> *)new_node),
                                            half_keys, elts_moved, 0);
                    }
                    // new elt goes into first node
                    if (rank + 1 <= curr_node->num_elts)
                    {
                        // release lock on the new node
                        if constexpr (traits::concurrent)
                        {
                            if (level > 0)
                            {
                                assert(new_node->level > 0);
                                ((BSkipNodeInternal<traits> *)(new_node))
                                    ->mutex_.write_unlock();
                            }
                            else
                            {
                                ((BSkipNodeLeaf<traits> *)(new_node))->mutex_.write_unlock();
                            }
                        }
                        assert(key < new_node->get_header());
                        assert(key > curr_node->get_header());
                        if (level > 0)
                        {
                            ((BSkipNodeInternal<traits> *)(curr_node))
                                ->insert_key_at_rank(rank + 1, key);
                        }
                        else
                        {
                            ((BSkipNodeLeaf<traits> *)(curr_node))
                                ->insert_elt_at_rank(rank + 1, k);
                        }
                        curr_node->num_elts++;

                        // if you are an internal node, make space for the new element's
                        // child
                        assert(num_split == 0);
                        if (level > 0)
                        {
                            assert(curr_node->level > 0);
                            BSkipNodeInternal<traits> *curr_node_cast =
                                (BSkipNodeInternal<traits> *)curr_node;
                            // point to the first split node
                            curr_node_cast->insert_child_at_rank(rank + 1,
                                                                 new_nodes[num_split]);
                            if constexpr (traits::concurrent)
                            {
                                parent_lock = &((curr_node_cast)->mutex_);
                                parent_node = curr_node_cast;
                            }
                            curr_node = curr_node_cast->get_child_at_rank(rank);
                        }
                    }
                    else
                    { // insert it into the new node
                        assert(key > new_node->get_header());
                        assert(new_node->num_elts > 0);
                        int rank_to_insert = rank - curr_node->num_elts;

                        // release left
                        if constexpr (traits::concurrent)
                        {
                            if (level > 0)
                            {
                                ((BSkipNodeInternal<traits> *)(curr_node))
                                    ->mutex_.write_unlock();
                            }
                            else
                            {
                                ((BSkipNodeLeaf<traits> *)(curr_node))->mutex_.write_unlock();
                            }
                        }
                        // insert into right
                        if (level > 0)
                        {
                            ((BSkipNodeInternal<traits> *)(new_node))
                                ->insert_key_at_rank(rank_to_insert + 1, key);
                        }
                        else
                        {
                            ((BSkipNodeLeaf<traits> *)(new_node))
                                ->insert_elt_at_rank(rank_to_insert + 1, k);
                        }
                        new_node->num_elts++;

                        if (level > 0)
                        {
                            BSkipNodeInternal<traits> *new_node_cast =
                                (BSkipNodeInternal<traits> *)new_node;
                            new_node_cast->insert_child_at_rank(rank_to_insert + 1,
                                                                new_nodes[num_split]);
                            if constexpr (traits::concurrent)
                            {
                                parent_node = new_node_cast;
                                parent_lock = &((new_node_cast)->mutex_);
                            }

                            // drop down to next level
                            curr_node = new_node_cast->get_child_at_rank(rank_to_insert);

                            assert(curr_node);
                            assert(curr_node->get_header() ==
                                   new_node_cast->get_child_at_rank(rank_to_insert)
                                       ->get_header());
                        }
                        else
                        {
                            curr_node = new_node;
                        }
                    }
                }
                else
                { // did not fill this node
                    assert(level_to_promote == level);
                    // add key (and if needed, value) to this node
                    if (level > 0)
                    {
                        ((BSkipNodeInternal<traits> *)curr_node)
                            ->insert_key_at_rank(rank + 1, key);
                    }
                    else
                    {
                        ((BSkipNodeLeaf<traits> *)curr_node)
                            ->insert_elt_at_rank(rank + 1, k);
                    }

                    // make space for the child pointer
                    if (level > 0)
                    {
                        BSkipNodeInternal<traits> *curr_node_cast =
                            (BSkipNodeInternal<traits> *)curr_node;
                        // update the down pointer for the new elt
                        assert(num_split == 0);
                        assert(rank < curr_node->num_elts);
                        curr_node_cast->insert_child_at_rank(rank + 1,
                                                             new_nodes[num_split], false);

                        // keep track of the lock
                        if constexpr (traits::concurrent)
                        {
                            parent_lock = &((curr_node_cast)->mutex_);
                            parent_node = curr_node_cast;
                        }
                        // update num elts
                        curr_node->num_elts++;
                        tbassert(curr_node->num_elts <= traits::MAX_KEYS, "num elts %u, max keys %lu\n", curr_node->num_elts, traits::MAX_KEYS);
                        // drop down
                        curr_node = curr_node_cast->get_child_at_rank(rank);
                        assert(curr_node != NULL);
                    }
                    else
                    {
                        curr_node->num_elts++;
                    }
                }
            }
            else
            { // case 3: do a split
                assert(level_to_promote > level);
                BSkipNode<traits> *new_node = new_nodes[num_split];
                num_split++;

                assert(new_node);

                // fixup pointer from new to next node
                new_node->next = curr_node->next;
                new_node->next_header = curr_node->next_header;

                // set the level
                new_node->level = level;

                // put the new key at the start of the new node
                if (level > 0)
                {
                    ((BSkipNodeInternal<traits> *)new_node)->insert_key_at_rank(0, key);
                }
                else
                {
                    ((BSkipNodeLeaf<traits> *)(new_node))->insert_elt_at_rank(0, k);
                }
                new_node->num_elts++;

                // move all keys in the curr node starting from rank + 1 into the new
                // node returns the number of elements that were moved updates the
                // number of elts in each node
                uint32_t elts_moved = curr_node->split_keys(new_node, rank + 1);

                // if you are an internal node, make space for the child of this head
                if (level > 0)
                {
                    ((BSkipNodeInternal<traits> *)new_node)
                        ->insert_child_at_rank(0, new_nodes[num_split]);

                    ((BSkipNodeInternal<traits> *)curr_node)
                        ->move_children(((BSkipNodeInternal<traits> *)new_node), rank + 1,
                                        elts_moved);
                }

                assert(curr_node->get_header() < curr_node->next->get_header());
                assert(new_node->get_header() < curr_node->next->get_header());
                assert(curr_node->get_header() < new_node->get_header());

                // fixup next pointers
                curr_node->next = new_node;
                curr_node->next_header = new_node->get_header();

                // unlock new node
                if constexpr (traits::concurrent)
                {
                    if (level > 0)
                    {
                        ((BSkipNodeInternal<traits> *)(new_node))->mutex_.write_unlock();
                    }
                    else
                    {
                        ((BSkipNodeLeaf<traits> *)(new_node))->mutex_.write_unlock();
                    }
                }

                // if we are at an internal level, move on and save info for lower
                // splits
                if (level > 0)
                {
                    // save parent lock

                    BSkipNodeInternal<traits> *curr_node_cast =
                        (BSkipNodeInternal<traits> *)curr_node;
                    if constexpr (traits::concurrent)
                    {
                        parent_lock = &(curr_node_cast->mutex_);
                        parent_node = curr_node_cast;
                    }

                    // drop down one level
                    curr_node = curr_node_cast->get_child_at_rank(rank);
                    assert(curr_node);
                }
            }
        }
        if (level == 0)
        {
            if constexpr (traits::concurrent)
            {
                ((BSkipNodeLeaf<traits> *)(curr_node))->mutex_.write_unlock();
            }
            assert(curr_node);
            return true;
        }
    }

    assert(num_split == level_to_promote);
    return true;
}

#include "instances.cpp"