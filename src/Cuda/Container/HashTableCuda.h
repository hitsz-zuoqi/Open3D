/**
 * Created by wei on 18-4-2.
 */

#pragma once

#include "ArrayCuda.h"
#include "LinkedListCuda.h"
#include "MemoryHeapCuda.h"

#include <Cuda/Common/Common.h>
#include <Cuda/Common/LinearAlgebraCuda.h>

namespace open3d {

namespace cuda {
/** TODO: implement Cuckoo Hashing **/

/**
 * Taken from Niessner, et al, 2013
 * Real-time 3D Reconstruction at Scale using Voxel Hashing
 */
class SpatialHasher {
private:
    int bucket_count_;

public:
    /* Some large default bucket size */
    __HOSTDEVICE__ SpatialHasher() { bucket_count_ = 1000000; }
    __HOSTDEVICE__ SpatialHasher(int bucket_count)
        : bucket_count_(bucket_count) {}
    __HOSTDEVICE__ inline size_t operator()(const Vector3i &key) const {
        const int p0 = 73856093;
        const int p1 = 19349669;
        const int p2 = 83492791;

        int r = ((key(0) * p0) ^ (key(1) * p1) ^ (key(2) * p2)) % bucket_count_;
        if (r < 0) r += bucket_count_;
        return (size_t) r;
    }
};

template<typename Key>
class HashEntry {
public:
    Key key;
    int internal_addr;

    __HOSTDEVICE__ inline bool operator==(const HashEntry<Key> &other) const {
        return key == other.key;
    }
    __HOSTDEVICE__ inline bool Matches(const Key &other) const {
        return (key == other) && (internal_addr != NULLPTR_CUDA);
    }
    __HOSTDEVICE__ inline bool IsEmpty() {
        return internal_addr == NULLPTR_CUDA;
    }
    __HOSTDEVICE__ inline void Clear() {
        key = Key();
        internal_addr = NULLPTR_CUDA;
    }
};

typedef HashEntry<Vector3i> SpatialEntry;

#define BUCKET_SIZE 10

/**
 * My implementation of K\"ahler, et al, 2015
 * Very High Frame Rate Volumetric Integration of Depth Images on Mobile Devices
 *  ordered (array)   unordered (linked list)
 * | | | | | | | --- | | | |
 * | | | | | | | --- | |
 * | | | | | | | --- | | | | | |
 */
template<typename Key, typename Value, typename Hasher>
class HashTableCudaDevice {
public:
    typedef HashEntry<Key> Entry;
    typedef LinkedListCudaDevice<Entry> LinkedListEntryCudaServer;
    typedef LinkedListNodeCuda<Entry> LinkedListNodeEntryCuda;
    int bucket_count_;

private:
    Hasher hasher_;

    /* bucket_count_ x BUCKET_SIZE */
    ArrayCudaDevice<Entry> entry_array_;
    /* bucket_count_ -> LinkedList */
    ArrayCudaDevice<LinkedListEntryCudaServer> entry_list_array_;
    /* Collect assigned entries for parallel processing */
    ArrayCudaDevice<Entry> assigned_entry_array_;

    ArrayCudaDevice<int> lock_array_;

    /* For managing LinkedListNodes and Values */
    MemoryHeapCudaDevice<Value> memory_heap_value_;
    MemoryHeapCudaDevice<LinkedListNodeEntryCuda> memory_heap_entry_list_node_;

    /** WARNING!!!
      * When our Cuda containers store SERVERS
      * (in this case LinkedListEntryCudaServer),
      * we have to be very careful.
      * - One option is to call Create() for their host classes per element.
      *   but that means, for a 100000 element array, we have to allocate 100000
      *   host classes them on CPU, create them, and push them on GPU
      *   one-by-one. That is too expensive and stupid.
      * - Another option is to allocate them on CUDA using malloc, but that
      *   is very slow (imagine thousands of kernel querying per element
      *   mallocing simultaneously.
      * - So we choose external allocation for LinkedLists, and manage them
      *   on kernels. */
    int *entry_list_head_node_ptrs_memory_pool_;
    int *entry_list_size_ptrs_memory_pool_;

    /** Internal implementations **/
    /**
    * @param key
    * @return ptr (stored in an int addr)
    * that could be accessed in @data_memory_heap_
    * Make it private to avoid confusion
    * between internal ptrs (MemoryHeap) and conventional ptrs (*Object)
    */
public:
    __DEVICE__ int GetInternalAddrByKey(const Key &key);
    __DEVICE__ Value *GetValuePtrByInternalAddr(const int addr);

    /** External interfaces - return nullable object **/
    __DEVICE__ Value *GetValuePtrByKey(const Key &key);
    __DEVICE__ Value *operator[](const Key &key);

    __DEVICE__ int New(const Key &key);
    __DEVICE__ int Delete(const Key &key);

    __DEVICE__ inline ArrayCudaDevice<Entry> &entry_array() {
        return entry_array_;
    }
    __DEVICE__ inline ArrayCudaDevice<LinkedListEntryCudaServer>
    &entry_list_array() {
        return entry_list_array_;
    }
    __DEVICE__ inline ArrayCudaDevice<Entry> &assigned_entry_array() {
        return assigned_entry_array_;
    }
    __DEVICE__ inline MemoryHeapCudaDevice<LinkedListNodeEntryCuda>
    &memory_heap_entry_list_node() {
        return memory_heap_entry_list_node_;
    }
    __DEVICE__ inline MemoryHeapCudaDevice<Value> &memory_heap_value() {
        return memory_heap_value_;
    }
    __DEVICE__ inline int *&entry_list_head_node_ptrs_memory_pool() {
        return entry_list_head_node_ptrs_memory_pool_;
    }
    __DEVICE__ inline int *&entry_list_size_ptrs_memory_pool() {
        return entry_list_size_ptrs_memory_pool_;
    }

    friend class HashTableCuda<Key, Value, Hasher>;
};

template<typename Key, typename Value, typename Hasher>
class HashTableCuda {
public:
    typedef HashEntry<Key> Entry;
    typedef LinkedListCudaDevice<Entry> LinkedListEntryCudaServer;
    typedef LinkedListNodeCuda<Entry> LinkedListNodeEntryCuda;

private:
    Hasher hasher_;

    MemoryHeapCuda<LinkedListNodeEntryCuda> memory_heap_entry_list_node_;
    MemoryHeapCuda<Value> memory_heap_value_;

    ArrayCuda<Entry> entry_array_;
    ArrayCuda<LinkedListEntryCudaServer> entry_list_array_;
    ArrayCuda<Entry> assigned_entry_array_;
    ArrayCuda<int> lock_array_;

    /* Wrap all above */
public:
    std::shared_ptr<HashTableCudaDevice<Key, Value, Hasher>> device_ = nullptr;

public:
    int bucket_count_;
    int max_value_capacity_;
    int max_linked_list_node_capacity_;

public:
    HashTableCuda();
    HashTableCuda(const HashTableCuda<Key, Value, Hasher> &other);
    HashTableCuda<Key, Value, Hasher> &operator=(
        const HashTableCuda<Key, Value, Hasher> &other);
    ~HashTableCuda();

    void Create(int bucket_count, int value_capacity);
    void Release();
    void UpdateDevice();

    void Reset();
    void ResetEntries();
    void ResetLocks();
    void GetAssignedEntries();

    /**
     * The internal data structure is too complicated to be separately dumped.
     * We try to pre-process them before dumping them to CPU.
     * @param pairs
     * @return pair count
     */
    void New(std::vector<Key> &keys, std::vector<Value> &values);
    void Delete(std::vector<Key> &keys);
    std::tuple<std::vector<int>, std::vector<int>> Profile();
    std::tuple<std::vector<Key>, std::vector<Value>> Download();
    std::vector<Entry> DownloadAssignedEntries();

    const Hasher &hasher() const {
        return hasher_;
    }
    const ArrayCuda<Entry> &entry_array() const {
        return entry_array_;
    }
    const ArrayCuda<LinkedListEntryCudaServer> &entry_list_array() const {
        return entry_list_array_;
    }
    const ArrayCuda<Entry> &assigned_entry_array() const {
        return assigned_entry_array_;
    };
    ArrayCuda<Entry> &assigned_entry_array() {
        return assigned_entry_array_;
    }
    const MemoryHeapCuda<LinkedListNodeEntryCuda> &
    memory_heap_entry_list_node() const {
        return memory_heap_entry_list_node_;
    }
    const MemoryHeapCuda<Value> &memory_heap_value() const {
        return memory_heap_value_;
    }
    const ArrayCuda<int> &lock_array() const {
        return lock_array_;
    }
};

template<typename Key, typename Value, typename Hasher>
class HashTableCudaKernelCaller {
public:
    static __HOST__ void CreateHashTableEntriesKernelCaller(
        HashTableCudaDevice<Key, Value, Hasher> &server,
        int bucket_count);
    static __HOST__ void ReleaseHashTableEntriesKernelCaller(
        HashTableCudaDevice<Key, Value, Hasher> &server,
        int bucket_count);

    static __HOST__ void ResetHashTableEntriesKernelCaller(
        HashTableCudaDevice<Key, Value, Hasher> &server,
        int bucket_count);

    static __HOST__ void GetHashTableAssignedEntriesKernelCaller(
        HashTableCudaDevice<Key, Value, Hasher> &server,
        int bucket_count);

    static __HOST__ void InsertHashTableEntriesKernelCaller(
        HashTableCudaDevice<Key, Value, Hasher> &server,
        ArrayCudaDevice<Key> &keys,
        ArrayCudaDevice<Value> &values,
        int num_pairs,
        int bucket_count);

    static __HOST__ void DeleteHashTableEntriesKernelCaller(
        HashTableCudaDevice<Key, Value, Hasher> &server,
        ArrayCudaDevice<Key> &keys, int num_keys, int
        bucket_count);

    static __HOST__ void ProfileHashTableKernelCaller(
        HashTableCudaDevice<Key, Value, Hasher> &server,
        ArrayCudaDevice<int> &array_entry_count,
        ArrayCudaDevice<int> &linked_list_entry_count,
        int bucket_count);

};

/** Memory management **/
template<typename Key, typename Value, typename Hasher>
__GLOBAL__
void CreateHashTableEntriesKernel(
    HashTableCudaDevice<Key, Value, Hasher> server);

template<typename Key, typename Value, typename Hasher>
__GLOBAL__
void ReleaseHashTableEntriesKernel(
    HashTableCudaDevice<Key, Value, Hasher> server);

template<typename Key, typename Value, typename Hasher>
__GLOBAL__
void ResetHashTableEntriesKernel(
    HashTableCudaDevice<Key, Value, Hasher> server);

template<typename Key, typename Value, typename Hasher>
__GLOBAL__
void GetHashTableAssignedEntriesKernel(
    HashTableCudaDevice<Key, Value, Hasher> server);

/** Insert **/
template<typename Key, typename Value, typename Hasher>
__GLOBAL__
void InsertHashTableEntriesKernel(
    HashTableCudaDevice<Key, Value, Hasher> server,
    ArrayCudaDevice<Key> keys, ArrayCudaDevice<Value> values, int num_pairs);

/** Delete **/
template<typename Key, typename Value, typename Hasher>
__GLOBAL__
void DeleteHashTableEntriesKernel(
    HashTableCudaDevice<Key, Value, Hasher> server, ArrayCudaDevice<Key> keys,
    int num_keys);

template<typename Key, typename Value, typename Hasher>
__GLOBAL__
void ProfileHashTableKernel(
    HashTableCudaDevice<Key, Value, Hasher> server,
    ArrayCudaDevice<int> array_entry_count,
    ArrayCudaDevice<int> linked_list_entry_count);
} // cuda
} // open3d