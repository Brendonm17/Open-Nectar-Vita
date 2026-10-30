#include "Node.h"
#include "DebugLog.h"
#include "Dolphin/os.h"
#include "system.h"
#include <stdio.h>

NodeMgr* nodeMgr;

int Node::currID;

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F0
 */
DEFINE_PRINT("Node");

#if defined(__vita__)
/* PS Vita: watched lists. */
struct PcWatchedList {
	CoreNode* mRoot;       // the list head, null for a free slot
	u32 mGen;              // never 0 once registered
	u32 mCursorGen;        // the cursor below is valid for this generation (0: none)
	int mCursorIdx;        // index of mCursorNode, as a walk from the head counts
	CoreNode* mCursorNode; // what that walk returned (null at the end)
	u32 mCountGen;         // mCount is valid for this generation (0: none)
	int mCount;            // getChildCount()
};
static const int kPcWatchedLists = 8;
static PcWatchedList sPcWatched[kPcWatchedLists];
static int sPcWatchedNext         = 0;
static u32 sPcGenSource           = 0;

static inline u32 pcNextGen()
{
	if (++sPcGenSource == 0) {
		sPcGenSource = 1;
	}
	return sPcGenSource;
}

static inline PcWatchedList* pcWatchedFind(CoreNode* root)
{
	for (int i = 0; i < kPcWatchedLists; i++) {
		if (sPcWatched[i].mRoot == root) {
			return &sPcWatched[i];
		}
	}
	return nullptr;
}

static inline void pcWatchedBump(CoreNode* root)
{
	if (!root) {
		return;
	}
	PcWatchedList* list = pcWatchedFind(root);
	if (list) {
		list->mGen = pcNextGen();
	}
}

// The original walk: idx steps from the head, a negative idx taking none.
static inline CoreNode* pcNodeWalk(CoreNode* root, int idx)
{
	CoreNode* node = root->mChild;
	for (int i = 0; i < idx; i++) {
		node = node->mNext;
	}
	return node;
}

/** @brief Registers a list head. */
void pcNodeListWatch(CoreNode* root)
{
	PcWatchedList* list = pcWatchedFind(root);
	if (!list) {
		list           = &sPcWatched[sPcWatchedNext];
		sPcWatchedNext = (sPcWatchedNext + 1) % kPcWatchedLists;
	}
	list->mRoot       = root;
	list->mGen        = pcNextGen();
	list->mCursorGen  = 0;
	list->mCursorIdx  = 0;
	list->mCursorNode = nullptr;
	list->mCountGen   = 0;
	list->mCount      = 0;
}

/** @brief Marks a watched head changed. */
void pcNodeListTouched(CoreNode* root)
{
	pcWatchedBump(root);
}

/** @brief The head's current generation, or 0 if not watched. */
u32 pcNodeListGen(CoreNode* root)
{
	PcWatchedList* list = pcWatchedFind(root);
	return list ? list->mGen : 0;
}

/** @brief The node idx steps from root->mChild, cached for in-order stepping. */
CoreNode* pcNodeListAt(CoreNode* root, int idx)
{
	PcWatchedList* list = (idx >= 0) ? pcWatchedFind(root) : nullptr;
	if (!list) {
		return pcNodeWalk(root, idx);
	}
	CoreNode* node;
	if (list->mCursorGen == list->mGen && idx == list->mCursorIdx) {
		node = list->mCursorNode;
	} else if (list->mCursorGen == list->mGen && idx == list->mCursorIdx + 1 && list->mCursorNode) {
		node = list->mCursorNode->mNext;
	} else {
		node = pcNodeWalk(root, idx);
	}
	list->mCursorGen  = list->mGen;
	list->mCursorIdx  = idx;
	list->mCursorNode = node;
	return node;
}

/** @brief Cached root->getChildCount(). */
int pcNodeListCount(CoreNode* root)
{
	PcWatchedList* list = pcWatchedFind(root);
	if (!list) {
		return root->getChildCount();
	}
	if (list->mCountGen != list->mGen) {
		list->mCount    = root->getChildCount();
		list->mCountGen = list->mGen;
	}
	return list->mCount;
}
#endif

/**
 * @todo: Documentation
 */
void CoreNode::add(CoreNode* toAdd)
{
	CoreNode* child = mChild;

	// If there is a child
	if (child) {
		// Find the last child
		while (child->mNext) {
			child = child->mNext;
		}

		//  Add the new child to the end
		child->mNext = toAdd;
	} else {
		// If there is no child, add the new child
		mChild = toAdd;
	}

	// 	Set the parent of the new child to this node
	toAdd->mParent = this;
#if defined(__vita__)
	pcWatchedBump(this);
#endif
}

/**
 * @todo: Documentation
 */
void CoreNode::del()
{
#if defined(__vita__)
	pcWatchedBump(mParent);
#endif
	// If there's no parent, there's nothing to delete
	if (mParent) {
		CoreNode* child = mParent->mChild;
		CoreNode* prev  = nullptr;

		// Iterate through the children to find the one to delete
		for (child; child; prev = child, child = child->mNext) {
			if (child == this) {
				if (prev) {
					// don't break the list, link prev to next
					prev->mNext = child->mNext;

					// Detach the current child
					mNext   = nullptr;
					mParent = nullptr;
					return;
				}
				// If there's no previous child, set the parent's child to the next child
				mParent->mChild = child->mNext;

				// Detach the current child
				mNext   = nullptr;
				mParent = nullptr;
				return;
			}
		}
	}
}

/**
 * @todo: Documentation
 */
int CoreNode::getChildCount()
{
	int count = 0;
	FOREACH_NODE(CoreNode, mChild, node)
	{
		count++;
	}
	return count;
}

/**
 * @todo: Documentation
 */
void CoreNode::load(immut char* dirPath, immut char* fileName, u32)
{
	char filePath[PATH_MAX];
	sprintf(filePath, "%s%s", dirPath, fileName);
	RandomAccessStream* stream = gsys->openFile(filePath);
	if (stream) {
		read(*stream);
		stream->close();
	}
}

/**
 * @todo: Documentation
 */
void Node::init(immut char* name)
{
	initCore(name);
	mFlags = 0x1 | 0x2;
	_1C    = -1;
}

/**
 * @todo: Documentation
 */
void Node::update()
{
	FOREACH_NODE(Node, mChild, node)
	{
		if (node->mFlags & 1) {
			node->concat();
			node->update();
		}
	}
}

/**
 * @todo: Documentation
 */
void Node::draw(Graphics& gfx)
{
	FOREACH_NODE(Node, mChild, node)
	{
		node->draw(gfx);
	}
}

/**
 * @todo: Documentation
 */
void Node::render(Graphics& gfx)
{
	draw(gfx);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000128 (Matching by size)
 */
SRTNode::SRTNode(immut char* name = "<SRTNode>")
    : Node(name)
{
	setPosition(Vector3f(0.0f, 0.0f, 0.0f));
	setScale(Vector3f(1.0f, 1.0f, 1.0f));
	setRotation(Vector3f(0.0f, 0.0f, 0.0f));
}

/**
 * @todo: Documentation
 */
void SRTNode::update()
{
	// this feels like it should be SRTNode, but it's not
	// if it were, SRTNode::concat would generate after this function rather than the end
	FOREACH_NODE(Node, mChild, node)
	{
		if (node->mFlags & 1) {
			node->concat(mWorldMtx);
			node->update();
		}
	}
}

/**
 * @todo: Documentation
 */
void NodeMgr::Del(Node* node)
{
	mDelete = true;
	if (!node) {
		return;
	}
	if (!node->mParent) {
		return;
	}
#if defined(__vita__)
	pcWatchedBump(node->mParent);
#endif

	Node* child = static_cast<Node*>(node->mParent->mChild);
	Node* prev  = nullptr;
	for (child; child; prev = child, child = static_cast<Node*>(child->mNext)) {
		if (child == node) {
			if (prev) {
				prev->mNext = child->mNext;
				return;
			}

			node->mParent->mChild = child->mNext;
		}
	}
}

static bool foundNode;
static CoreNode* nodeFound;

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000EC (Matching by size)
 * @warning Broken and thread-unsafe
 */
void NodeMgr::recFindNode(CoreNode* head, immut char* name)
{
	// Shouldn't this condition be somewhere inside the loop?  It only
	// prevents the recursion from going deeper, meaning other sibling
	// nodes can still hijack the node found before the loops conclude.
	if (!foundNode) {
		FOREACH_NODE_ALT(CoreNode, head, currNode)
		{
			if (!strcmp(name, currNode->mName)) {
				foundNode = true;
				nodeFound = currNode;
				return;
			}
			if (currNode->mChild) {
				recFindNode(currNode->mChild, name);
			}
		}
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000A8 (Matching by size)
 * @warning Broken and thread-unsafe
 */
CoreNode* NodeMgr::findNode(immut char* name, CoreNode* head)
{
	nodeFound = nullptr;
	foundNode = false;

	if (!head) {
		head = firstNode();
	}
	recFindNode(head, name);

	return nodeFound;
}

/**
 * @todo: Documentation
 */
NodeMgr::NodeMgr()
{
	mDelete = false;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000040 (Matching by size)
 */
NodeMgr::~NodeMgr()
{
}
