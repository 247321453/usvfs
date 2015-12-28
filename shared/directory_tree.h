/*
Userspace Virtual Filesystem

Copyright (C) 2015 Sebastian Herbord. All rights reserved.

This file is part of usvfs.

usvfs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

usvfs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with usvfs. If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include "wildcard.h"
#include "shared_memory.h"
#include "scopeguard.h"
#include "logging.h"
#include <boost/predef.h>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>
#include "exceptionex.h"
#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/containers/map.hpp>
#include <boost/interprocess/containers/vector.hpp>
#include <boost/interprocess/offset_ptr.hpp>
#include <boost/interprocess/smart_ptr/shared_ptr.hpp>
#include <boost/interprocess/smart_ptr/weak_ptr.hpp>
#include <boost/interprocess/smart_ptr/deleter.hpp>
#include <boost/interprocess/sync/named_mutex.hpp>
#include <map>
#include <memory>
#include <regex>
#include <functional>
#include <iomanip>
#include <memory>
#include <cstdint>
#include <spdlog.h>



// simplify unit tests by allowing access to private members
#ifndef PRIVATE
#define PRIVATE private
#endif // PRIVATE


namespace usvfs {

namespace shared {

template <typename T, typename U>
struct SHMDataCreator {
  static T create(const U &source, const VoidAllocatorT &allocator) {
    return T(source, allocator);
  }
};

template <typename T, typename U> T createData(const U &source, const VoidAllocatorT &allocator)
{
  return SHMDataCreator<T, U>::create(source, allocator);
}

template <typename T> T createDataEmpty(const VoidAllocatorT &allocator);

template <typename T> void dataAssign(T &destination, const T &source);

// crappy little workaround for boost::filesystem::path iterating over path separators
boost::filesystem::path::iterator nextIter(const boost::filesystem::path::iterator &iter,
                                           const boost::filesystem::path::iterator &end);


namespace bi = boost::interprocess;

typedef uint8_t TreeFlags;


static const TreeFlags FLAG_DIRECTORY     = 0x01;
static const TreeFlags FLAG_DUMMY         = 0x02;
static const TreeFlags FLAG_FIRSTUSERFLAG = 0x10;

struct MissingThrowT {};
static const MissingThrowT MissingThrow = MissingThrowT();


template <typename NodeDataT> class TreeContainer;

/**
 * a representation of a directory tree in memory.
 * This class is designed to be stored in sharedG memory.
 */
template <typename NodeDataT>
class DirectoryTree
{
  template <typename T>
  friend class TreeContainer;

protected:

  struct CILess
  {
    bool operator() (const StringT &lhs, const StringT &rhs) const
    {
      return _stricmp(lhs.c_str(), rhs.c_str()) < 0;
    }
  };

public:

  typedef DirectoryTree<NodeDataT> NodeT;
  typedef bi::deleter<NodeT, SegmentManagerT> DeleterT;
  typedef NodeDataT DataT;

  typedef bi::shared_ptr<NodeT, VoidAllocatorT, DeleterT> NodePtrT;
  typedef bi::weak_ptr<NodeT, VoidAllocatorT, DeleterT> WeakPtrT;

  typedef bi::allocator<std::pair<const StringT, NodePtrT>, SegmentManagerT> NodeEntryAllocatorT;

  typedef bi::map<StringT, NodePtrT, CILess, NodeEntryAllocatorT> NodeMapT;
  typedef typename NodeMapT::iterator file_iterator;
  typedef typename NodeMapT::const_iterator const_file_iterator;

  typedef std::function<void (const NodePtrT&)> VisitorFunction;

public:

  DirectoryTree() = delete;

  DirectoryTree(const NodeT &reference) = delete;

  /**
   * @brief construct a new node to be inserted in an existing tree
   **/
  DirectoryTree(const std::string &name
                , TreeFlags flags
                , NodePtrT parent
                , const NodeDataT &data
                , const VoidAllocatorT &allocator)
    : m_Parent(parent)
    , m_Name(name.c_str(), allocator)
    , m_Data(data)
    , m_Nodes(allocator)
    , m_Flags(flags)
  {
  }

  /**
   * @brief move constructor
   * @param reference source tree to move from
   */
  DirectoryTree(NodeT &&reference) = delete;

  ~DirectoryTree() {
    m_Nodes.clear();
  }

  /**
   * @brief assignment operator
   */
  NodeT &operator=(NodeT reference) = delete;

  /**
   * @return parent node
   */
  NodePtrT parent() const { return m_Parent.lock(); }

  /**
   * @return the full path to the node
   */
  boost::filesystem::path path() const {
    if (m_Parent.lock().get() == nullptr) {
      if (m_Name.size() == 0) {
        return boost::filesystem::path();
      } else {
        return boost::filesystem::path(m_Name.c_str()) / "\\";
      }
    } else {
      return m_Parent.lock()->path() / m_Name.c_str();
    }
  }

  /**
   * @return data connected to this node
   **/
  const NodeDataT &data() const {
    return m_Data;
  }

  /**
   * @return name of this node
   */
  std::string name() const { return m_Name.c_str(); }

  /**
   * @brief setFlag change a flag for this node
   * @param enabled new state for the specified flag
   */
  void setFlag(TreeFlags flag, bool enabled = true) { m_Flags = enabled ? m_Flags | flag
                                                                        : m_Flags & ~flag; }

  /**
   * @return true if the specified flag is set, false otherwise
   */
  bool hasFlag(TreeFlags flag) const { return (m_Flags & flag) != 0; }

  /**
   * @return true if this node is a directory, false if it's a regular file
   */
  bool isDirectory() const { return hasFlag(FLAG_DIRECTORY); }

  /**
   * @return the number of subnodes (directly) below this one
   */
  size_t numNodes() const { return m_Nodes.size(); }

  /**
   * @return number of nodes in this (sub-)tree including this one
   */
  size_t numNodesRecursive() const {
    size_t result { numNodes() + 1 };
    for (const auto &node : m_Nodes) {
      result += node.second->numNodesRecursive();
    }
    return result;
  }

  /**
   * @brief find a node by its path
   * @param path the path to look up
   * @return a pointer to the node or a null ptr
   */
  NodePtrT findNode(const boost::filesystem::path &path) {
    return findNode(path, path.begin());
  }

  /**
   * @brief find a node by its path
   * @param path the path to look up
   * @return a pointer to the node or a null ptr
   */
  const NodePtrT findNode(const boost::filesystem::path &path) const {
    return findNode(path, path.begin());
  }

  /**
   * @brief visit the nodes along the specified path (in order) calling the visitor for each
   * @param path the path to visit
   * @param visitor a function called for each node
   */
  void visitPath(const boost::filesystem::path &path, const VisitorFunction &visitor) const {
    visitPath(path, path.begin(), visitor);
  }

  /**
   * @brief retrieve a node by the specified name
   * @param name name of the node
   * @return the node found or an empty pointer if no such node was found
   */
  NodePtrT node(const char *name, MissingThrowT) const {
    auto iter = m_Nodes.find(StringT(name, m_Nodes.get_allocator()));
    if (iter != m_Nodes.end()) {
      return iter->second;
    } else {
      USVFS_THROW_EXCEPTION(node_missing_error());
    }
  }

  /**
   * @brief retrieve a node by the specified name
   * @param name name of the node
   * @return the node found or an empty pointer if no such node was found
   */
  NodePtrT node(const char *name) {
    auto iter = m_Nodes.find(StringT(name, m_Nodes.get_allocator()));
    if (iter != m_Nodes.end()) {
      return iter->second;
    } else {
      return NodePtrT();
    }
  }

  /**
   * @brief retrieve a node by the specified name
   * @param name name of the node
   * @return the node found or an empty pointer if no such node was found
   */
  const NodePtrT node(const char *name, MissingThrowT) {
    auto iter = m_Nodes.find(StringT(name, m_Nodes.get_allocator()));
    if (iter != m_Nodes.end()) {
      return iter->second;
    } else {
      USVFS_THROW_EXCEPTION(node_missing_error());
    }
  }

  /**
   * @brief retrieve a node by the specified name
   * @param name name of the node
   * @return the node found or an empty pointer if no such node was found
   */
  const NodePtrT node(const char *name) const {
    auto iter = m_Nodes.find(StringT(name, m_Nodes.get_allocator()));
    if (iter != m_Nodes.end()) {
      return iter->second;
    } else {
      return NodePtrT();
    }
  }

  /**
   * @brief test if a node by the specified name exists
   * @param name name of the node
   * @return true if the node exists, false otherwise
   */
  bool exists(const char *name) const {
    return m_Nodes.find(StringT(name, m_Nodes.get_allocator())) != m_Nodes.end();
  }

  /**
   * @brief find all matches for a pattern
   * @param pattern the pattern to look for
   * @return a vector of the found nodes
   */
  std::vector<NodePtrT> find(const std::string &pattern) const {
    // determine if there is a prefix in the pattern that indicates a specific directory.
    size_t fixedPart = pattern.find_first_of("*?");
    if (fixedPart == 0)
      fixedPart = std::string::npos;
    if (fixedPart != std::string::npos)
      fixedPart = pattern.find_last_of(R"(\/)", fixedPart);
    std::vector<NodePtrT> result;

    if (fixedPart != std::string::npos) {
      // if there is a prefix, search for the node representing that path and search
      // only on that
      NodePtrT node = findNode(boost::filesystem::path(pattern.substr(0, fixedPart)));
      if (node.get() != nullptr) {
        node->findLocal(result, pattern.substr(fixedPart + 1));
      }
    } else {
      findLocal(result, pattern);
    }
    return result;
  }

  /**
   * @return an iterator to the first leaf
   **/
  file_iterator filesBegin() { return m_Nodes.begin(); }

  /**
   * @return a const iterator to the first leaf
   **/
  const_file_iterator filesBegin() const { return m_Nodes.begin(); }

  /**
   * @return an iterator one past the last leaf
   **/
  file_iterator filesEnd() { return m_Nodes.end(); }

  /**
   * @return a const iterator one past the last leaf
   **/
  const_file_iterator filesEnd() const { return m_Nodes.end(); }

  /**
   * @brief erase the leaf at the specfied iterator
   * @return an iterator to the following file
   **/
  file_iterator erase(file_iterator iter) { return m_Nodes.erase(iter); }

  /**
   * @brief clear all nodes
   */
  void clear() {
    m_Nodes.clear();
  }

PRIVATE:

  WeakPtrT findRoot() const
  {
    if (m_Parent.lock().get() == nullptr) {
      return m_Self;
    } else {
      return m_Parent.lock()->findRoot();
    }
  }

  NodePtrT findNode(const boost::filesystem::path &name, boost::filesystem::path::iterator iter) {
    auto subNode = m_Nodes.find(StringT(iter->string().c_str(), m_Nodes.get_allocator()));
    boost::filesystem::path::iterator next = nextIter(iter, name.end());
    if (next == name.end()) {
      // last name component, should be a local node
      if (subNode != m_Nodes.end()) {
        return subNode->second;
      } else {
        return NodePtrT();
      }
    } else {
      if (subNode != m_Nodes.end()) {
        return subNode->second->findNode(name, next);
      } else {
        return NodePtrT();
      }
    }
  }

  const NodePtrT findNode(const boost::filesystem::path &name, boost::filesystem::path::iterator iter) const {
    auto subNode = m_Nodes.find(StringT(iter->string().c_str(), m_Nodes.get_allocator()));
    boost::filesystem::path::iterator next = nextIter(iter, name.end());
    if (next == name.end()) {
      // last name component, should be a local node
      if (subNode != m_Nodes.end()) {
        return subNode->second;
      } else {
        return NodePtrT();
      }
    } else {
      if (subNode != m_Nodes.end()) {
        return subNode->second->findNode(name, next);
      } else {
        return NodePtrT();
      }
    }
  }

  void visitPath(const boost::filesystem::path &path
                 , boost::filesystem::path::iterator iter
                 , const VisitorFunction &visitor) const {
    auto subNode = m_Nodes.find(StringT(iter->string().c_str(), m_Nodes.get_allocator()));
    if (subNode != m_Nodes.end()) {
      visitor(subNode->second);
      auto next = nextIter(iter, path.end());
      if (next != path.end()) {
        subNode->second->visitPath(path, next, visitor);
      }
    }
  }

  void findLocal(std::vector<NodePtrT> &output, const std::string &pattern) const {
    for (auto iter = m_Nodes.begin(); iter != m_Nodes.end(); ++iter) {
      LPCSTR remainder = nullptr;
      if (   pattern.size() > 1
             && (pattern[0] == '*')
             && ((pattern[1] == '/')
                 || (pattern[1] == '\\'))
             && iter->second->isDirectory()) {
        // the star may represent a directory (one directory level, not multiple!), search in subdirectory
        iter->second->findLocal(output, pattern.substr(1));
      } else if ((remainder = wildcard::PartialMatch(iter->second->name().c_str(), pattern.c_str())) != nullptr) {
        if (   (*remainder == '\0')
            || (strcmp(remainder, "*") == 0)) {
          NodePtrT node = iter->second;
          output.push_back(node);
        }
        if (iter->second->isDirectory()) {
          iter->second->findLocal(output, remainder);
        }
      }
    }
  }

PRIVATE:

  TreeFlags m_Flags;

  WeakPtrT m_Parent;
  WeakPtrT m_Self;

  StringT m_Name;
  NodeDataT m_Data;

  NodeMapT m_Nodes;

};


/**
 * smart pointer to DirectoryTrees (only intended for top-level nodes). This will
 * transparently switch to new shared memory regions in case
 * they get reallocated
 */
template <typename TreeT>
class TreeContainer {

public:

//  static const char LockName[];

public:

  /**
   * @brief Constructor
   * @param SHMName name of the shared memory holding the tree. This should contain the running number
   * @param size initial size in bytes of the container. since the tree is resized by doubling this should be
   *        a power of two. 64k is supposed to be the page size on windows so smaller allocations make little sense
   * @note size can't be too small. If initial allocations fail automatic growing won't work
   */
  TreeContainer(const std::string &SHMName, size_t size = 64 * 1024)
    : m_TreeMeta(nullptr)
    , m_SHMName(SHMName)
  {
    namespace sp = std::placeholders;
    std::regex pattern(R"exp((.*_)(\d+))exp");
    std::smatch match;
    regex_match(std::string(m_SHMName.c_str()), match, pattern);
    if (match.size() != 3) {
      m_SHMName += "_1";
    }

    m_TreeMeta = createOrOpen(m_SHMName.c_str(), size);
    spdlog::get("usvfs")->info("attached to {0} with {1} nodes, size {2}",
                               m_SHMName, m_TreeMeta->tree->numNodesRecursive(),
                               m_SHM->get_size());
  }

  ~TreeContainer() {
    if (unassign(m_SHM, m_TreeMeta)) {
      bi::shared_memory_object::remove(m_SHMName.c_str());
    }
  }

  /**
   * @return retrieve an allocater that can be used to create objects in this tree
   */
  VoidAllocatorT allocator() {
    return VoidAllocatorT(m_SHM->get_segment_manager());
  }

  template <typename... Arguments>
  typename TreeT::DataT create(Arguments&&... args) {
    return TreeT::DataT(std::forward<Arguments>(args)..., allocator());
  }

  TreeT *operator->() {
    return get();
  }

  /**
   * @return raw pointer to the managed tree
   */
  TreeT *get() {
    if (m_TreeMeta->outdated) {
      reassign();
    }
    return m_TreeMeta->tree.get();
  }

  /**
   * @return raw const pointer to the managed tree
   */
  const TreeT *get() const {
    if (m_TreeMeta->outdated) {
      reassign();
    }
    return m_TreeMeta->tree.get();
  }

  const TreeT *operator->() const {
    return get();
  }

  /**
   * @return current name of the managed shared memory
   */
  std::string shmName() const {
    return m_SHMName;
  }

  void clear() {
    m_TreeMeta->tree->clear();
  }

  /**
   * @brief add a new file to the tree
   *
   * @param name name of the file, expected to be relative to this directory
   * @param data the file data to attach
   * @param flags flags for this files
   * @param overwrite if true, the new leaf will overwrite an existing one that compares as "equal"
   * @return pointer to the new node or a null ptr
   **/
  template <typename T>
  typename TreeT::NodePtrT addFile(const boost::filesystem::path &name
                                   , const T &data
                                   , TreeFlags flags = 0
                                   , bool overwrite = true) {
    namespace sp = std::placeholders;
    try {
      return addNode(m_TreeMeta->tree.get(), name, name.begin(),
                     data, overwrite, flags, allocator());
    } catch (const bi::bad_alloc&) {
      reassign();
      return addFile(name, data, flags, overwrite);
    }
  }

  /**
   * @brief add a new directory to the tree
   *
   * @param name name of the file, expected to be relative to this directory
   * @param data the file data to attach
   * @param flags flags for this files
   * @param overwrite if true, the new leaf will overwrite an existing one that compares as "equal"
   * @return pointer to the new node or a null ptr
   **/
  template <typename T>
  typename TreeT::NodePtrT addDirectory(const boost::filesystem::path &name, const T &data,
                                        TreeFlags flags = 0, bool overwrite = true) {
    using namespace std::placeholders;
    try {
      return addNode(m_TreeMeta->tree.get(), name, name.begin(), data, overwrite,
                     flags | FLAG_DIRECTORY, allocator());
    } catch (const bi::bad_alloc&) {
      reassign();
      return addDirectory(name, data, flags, overwrite);
    }
  }

private:

  struct TreeMeta {
    TreeMeta(const typename TreeT::DataT &data, SegmentManagerT *segmentManager)
      : tree(segmentManager->construct<TreeT>(bi::anonymous_instance)(
             "", true, TreeT::NodePtrT(), data, VoidAllocatorT(segmentManager)))
    { }
    bi::offset_ptr<TreeT, std::int32_t, std::uint64_t, 4> tree;
    long referenceCount { 0 }; // reference count only set on top level node
    bool outdated { false };

    bi::interprocess_mutex mutex;
  };

private:

  typename TreeT::DataT createEmpty() {
    return createDataEmpty<typename TreeT::DataT>(allocator());
  }

  template <typename T>
  TreeT *createSubNode(const VoidAllocatorT &allocator
                                , const std::string &name
                                , unsigned long flags
                                , const T &data)
  {
    SharedMemoryT::segment_manager *manager = allocator.get_segment_manager();


    return manager->construct<TreeT>(bi::anonymous_instance)(
        name
        , flags
        , TreeT::NodePtrT()
        , createData<TreeT::DataT, T>(data, allocator)
        , manager);

    // this seems to cause slightly less overhead but causes crashes on
    // x64 builds during drestruction of the TreeT
/*    void *mem = manager->allocate(sizeof(TreeT));
    return new(mem) TreeT(name, flags, TreeT::NodePtrT(), createData<TreeT::DataT, T>(data, allocator), manager);*/
  }

  typename TreeT::NodePtrT createSubPtr(TreeT *subNode)
  {
    SharedMemoryT::segment_manager *manager = m_SHM->get_segment_manager();
    return TreeT::NodePtrT(subNode, allocator(), TreeT::DeleterT(manager));
  }

  template <typename T>
  typename TreeT::NodePtrT addNode(DirectoryTree<typename TreeT::DataT> *base
                                   , const boost::filesystem::path &name
                                   , boost::filesystem::path::iterator iter
                                   , const T &data
                                   , bool overwrite
                                   , unsigned int flags
                                   , const VoidAllocatorT &allocator) {
    boost::filesystem::path::iterator next = nextIter(iter, name.end());
    StringT iterString(iter->string().c_str(), allocator);
    if (next == name.end()) {
      typename TreeT::NodePtrT newNode = this->get()->node(iter->string().c_str());

      if (newNode.get() == nullptr) {
        // last name component, should be the filename
        TreeT *node = createSubNode(allocator, iter->string(), flags, data);

        newNode = createSubPtr(node);
      }
      newNode->m_Self = TreeT::WeakPtrT(newNode);

      newNode->m_Parent = base->m_Self;

      if (overwrite) {
        base->m_Nodes[iterString] = newNode;
        return newNode;
      } else {
        auto res = base->m_Nodes.insert(std::make_pair(iterString, newNode));
        return res.second ? newNode : TreeT::NodePtrT();
      }
    } else {
      // not last component, continue search in child node
      auto subNode = base->m_Nodes.find(iterString);
      if (subNode == base->m_Nodes.end()) {
        typename TreeT::NodePtrT newNode = createSubPtr(createSubNode(allocator
                                                                      , iter->string()
                                                                      , FLAG_DIRECTORY | FLAG_DUMMY
                                                                      , createEmpty()));
        subNode = base->m_Nodes.insert(std::make_pair(iterString, newNode)).first;
        subNode->second->m_Self = TreeT::WeakPtrT(subNode->second);
        subNode->second->m_Parent = base->m_Self;
      }
      return addNode(subNode->second.get().get(), name, next, data, overwrite, flags, allocator);
    }
  }

  /**
   * @brief copy content of one tree to a different tree (in a different shared memory segment
   * @param destination
   * @param reference
   * @note at the time this is called, destination needs to refer to the shm of "destination" so that
   *       objects can be allocated in the new tree
   */
  void copyTree(TreeT *destination, const TreeT *reference) {
    VoidAllocatorT allocator = VoidAllocatorT(m_SHM->get_segment_manager());
    destination->m_Flags = reference->m_Flags;
    dataAssign(destination->m_Data, reference->m_Data);
    destination->m_Name.assign(reference->m_Name.c_str());
    for (const auto &kv : reference->m_Nodes) {
      TreeT *newNode = createSubNode(allocator, "", true, createEmpty());
      typename TreeT::NodePtrT newNodePtr = createSubPtr(newNode);
      // need to set self BEFORE recursively copying the subtree, otherwise how would we assign parent pointers?
      newNode->m_Self = newNodePtr;
      TreeT *source = reinterpret_cast<TreeT*>(kv.second.get().get());
      copyTree(newNode, source);
      destination->m_Nodes[newNode->m_Name] = newNodePtr;
      newNode->m_Parent = destination->m_Self;
    }
  }

  int increaseRefCount(TreeMeta *treeMeta) {
    bi::scoped_lock<bi::interprocess_mutex> lock(treeMeta->mutex);
    return ++treeMeta->referenceCount;
  }

  int decreaseRefCount(TreeMeta *treeMeta) {
    bi::scoped_lock<bi::interprocess_mutex> lock(treeMeta->mutex);
    return --treeMeta->referenceCount;
  }

  TreeMeta *createOrOpen(const char *SHMName, size_t size)
  {
//    bi::named_mutex mutex(bi::open_or_create, LockName);
//    bi::scoped_lock<bi::named_mutex> lock(mutex, boost::get_system_time() + boost::posix_time::seconds(1));

    SharedMemoryT *newSHM;
    try {
      newSHM = new SharedMemoryT(bi::open_only, SHMName);
      spdlog::get("usvfs")->info("{} opened in process {}",
                                 SHMName, ::GetCurrentProcessId());
    } catch (const bi::interprocess_exception&) {
      newSHM = new SharedMemoryT(bi::create_only, SHMName, static_cast<unsigned int>(size));
      spdlog::get("usvfs")->info("{} created in process {}",
                                 SHMName, ::GetCurrentProcessId());
    }
    return activateSHM(newSHM, SHMName);
  }

  TreeMeta *activateSHM(SharedMemoryT *shm, const char *SHMName)
  {
    spdlog::get("usvfs")->info("name: {0} - {2:p} -> {1:p}", SHMName, (void*)shm, (void*)m_SHM.get());

    std::shared_ptr<SharedMemoryT> oldSHM = m_SHM;

    m_SHM.reset(shm);
    std::pair<TreeMeta*, SharedMemoryT::size_type> res = m_SHM->find<TreeMeta>("Meta");
    bool lastUser = false;
    if (res.first == nullptr) {
      res.first = m_SHM->construct<TreeMeta>("Meta")(createEmpty(), m_SHM->get_segment_manager());
      if (res.first == nullptr) {
        USVFS_THROW_EXCEPTION(bi::bad_alloc());
      }
      if (m_TreeMeta != nullptr) {
        copyTree(res.first->tree.get(), m_TreeMeta->tree.get());
      }
    }
    increaseRefCount(res.first);

    if (oldSHM.get() != nullptr) {
      lastUser = unassign(oldSHM, m_TreeMeta);
    }

    if (lastUser) {
      // remove the !old! shm
      bi::shared_memory_object::remove(m_SHMName.c_str());
    }

    m_SHMName = SHMName;

    return res.first;
  }

  std::string followupName() const
  {
    std::regex pattern(R"exp((.*_)(\d+))exp");
    std::string shmName(m_SHMName.c_str()); // need to copy because the regex result will be iterators into this string
    std::smatch match;
    regex_match(shmName, match, pattern);
    if (match.size() != 3) {
      USVFS_THROW_EXCEPTION(usage_error() << ex_msg("shared memory name invalid"));
    }
    int count = boost::lexical_cast<int>(match[2]);

    return match[1].str() + std::to_string(count + 1);
  }

  bool unassign(const std::shared_ptr<SharedMemoryT> &shm, TreeMeta *tree)
  {
    if (decreaseRefCount(tree) == 0) {
      shm->get_segment_manager()->destroy_ptr(tree);
      return true;
    } else {
      return false;
    }
  }

  void reassign() const
  {
    // TODO evil const cast. We need to be able to reassign, even if the user only
    // has a read-lock on the tree because another process might have invalidated it.
    // This is not the solution
    auto *self = const_cast<TreeContainer<TreeT>*>(this);

    self->m_TreeMeta->outdated = true;

    for (;;) {
      std::string nextName = followupName();
      self->m_TreeMeta = self->createOrOpen(nextName.c_str(),
                                            m_SHM->get_size() * 2);

      if (!m_TreeMeta->outdated) {
        break;
      }
    }
    spdlog::get("usvfs")->info("tree {0} size now {1} bytes",
                               m_SHMName, m_SHM->get_size());
  }

private:

  std::string m_SHMName;
  std::shared_ptr<SharedMemoryT> m_SHM;
  TreeMeta *m_TreeMeta;

};


/*
template<typename NodeDataT>
const char TreeContainer<NodeDataT>::LockName[] = "tree_creation_lock";
*/

template <typename NodeDataT>
void dumpTree(std::ostream &stream, const DirectoryTree<NodeDataT> &tree,
              int level = 0)
{
  stream << std::string(level, ' ') << tree.name() << " -> " << tree.data()
         << "\n";
  for (auto iter = tree.filesBegin(); iter != tree.filesEnd(); ++iter) {
    dumpTree<NodeDataT>(stream, *iter->second, level + 1);
  }
}

} // namespace shared

} // namespace usvfs