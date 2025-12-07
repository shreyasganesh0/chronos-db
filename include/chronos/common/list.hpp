#pragma once

namespace chronos {

struct IntrusiveNode {

	IntrusiveNode *next = nullptr;
	IntrusiveNode *prev = nullptr;
};

template <typename T>
class IntrusiveList {
public:
	IntrusiveList() = default;

	IntrusiveList(const IntrusiveList&) = delete;
	IntrusiveList& operator=(const IntrusiveList&) = delete;

	bool empty() const {

		return head_ == nullptr;
	}

	void push_back(T *item) {

		if (empty()) {

			head_ = item;
			tail_ = item;

			item->prev = nullptr;
			item->next = nullptr;
		} else if (tail_ != nullptr) {

			tail_->next = item;
			item->prev = tail_;
			tail_ = item;
		}
	}

	T* pop_front() {
		if (empty()) return nullptr;

		IntrusiveNode* old_head = head_;
		head_ = head_->next;

		if (head_ != nullptr) {

			head_->prev = nullptr;
		} else {

			tail_ = nullptr;
		}

		old_head->next = nullptr;
		old_head->prev = nullptr;
		return static_cast<T*>(old_head);
	}
private:
	IntrusiveNode *head_ = nullptr;
	IntrusiveNode *tail_ = nullptr;
};
}
