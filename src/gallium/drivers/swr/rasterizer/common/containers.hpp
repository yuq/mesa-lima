/****************************************************************************
* Copyright (C) 2014-2015 Intel Corporation.   All Rights Reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice (including the next
* paragraph) shall be included in all copies or substantial portions of the
* Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
****************************************************************************/

#ifndef SWRLIB_CONTAINERS_HPP__
#define SWRLIB_CONTAINERS_HPP__

#include <functional>
#include "common/os.h"

namespace SWRL
{

template <typename T, int NUM_ELEMENTS>
struct UncheckedFixedVector
{
	UncheckedFixedVector() : mSize(0)
	{
	}

	UncheckedFixedVector(std::size_t size, T const& exemplar)
	{
		this->mSize = 0;
		for (std::size_t i = 0; i < size; ++i)
			this->push_back(exemplar);
	}

	template <typename Iter>
	UncheckedFixedVector(Iter fst, Iter lst)
	{
		this->mSize = 0;
		for ( ; fst != lst; ++fst)
			this->push_back(*fst);
	}

	UncheckedFixedVector(UncheckedFixedVector const& UFV)
	{
		this->mSize = 0;
		for (std::size_t i = 0, N = UFV.size(); i < N; ++i)
			(*this)[i] = UFV[i];
		this->mSize = UFV.size();
	}

	UncheckedFixedVector& operator=(UncheckedFixedVector const& UFV)
	{
		for (std::size_t i = 0, N = UFV.size(); i < N; ++i)
			(*this)[i] = UFV[i];
		this->mSize = UFV.size();
		return *this;
	}

	T* begin()	{ return &this->mElements[0]; }
	T* end()	{ return &this->mElements[0] + this->mSize; }
	T const* begin() const	{ return &this->mElements[0]; }
	T const* end() const	{ return &this->mElements[0] + this->mSize; }

	friend bool operator==(UncheckedFixedVector const& L, UncheckedFixedVector const& R)
	{
		if (L.size() != R.size()) return false;
		for (std::size_t i = 0, N = L.size(); i < N; ++i)
		{
			if (L[i] != R[i]) return false;
		}
		return true;
	}

	friend bool operator!=(UncheckedFixedVector const& L, UncheckedFixedVector const& R)
	{
		if (L.size() != R.size()) return true;
		for (std::size_t i = 0, N = L.size(); i < N; ++i)
		{
			if (L[i] != R[i]) return true;
		}
		return false;
	}

	T& operator[](std::size_t idx)
	{
		return this->mElements[idx];
	}
	T const& operator[](std::size_t idx) const
	{
		return this->mElements[idx];
	}
	void push_back(T const& t)
	{
		this->mElements[this->mSize]	= t;
		++this->mSize;
	}
	void pop_back()
	{
		SWR_ASSERT(this->mSize > 0);
		--this->mSize;
	}
	T& back()
	{
		return this->mElements[this->mSize-1];
	}
	T const& back() const
	{
		return this->mElements[this->mSize-1];
	}
	bool empty() const
	{
		return this->mSize == 0;
	}
	std::size_t size() const
	{
		return this->mSize;
	}
	void resize(std::size_t sz)
	{
		this->mSize = sz;
	}
	void clear()
	{
		this->resize(0);
	}
private:
	std::size_t	mSize;
	T			mElements[NUM_ELEMENTS];
};

template <typename T, int NUM_ELEMENTS>
struct FixedStack : UncheckedFixedVector<T, NUM_ELEMENTS>
{
	FixedStack() {}

	void push(T const& t)
	{
		this->push_back(t);
	}

	void pop()
	{
		this->pop_back();
	}

	T& top()
	{
		return this->back();
	}

	T const& top() const
	{
		return this->back();
	}
};

template <typename T>
struct CRCHash
{
    static_assert((sizeof(T) % sizeof(UINT)) == 0, "CRCHash expects templated type size is even multiple of 4B");
    UINT operator()(const T& k) const
    {
        UINT *pData = (UINT*)&k;
        UINT crc = 0;
        for (UINT i = 0; i < sizeof(T) / sizeof(UINT); ++i)
        {
            crc = _mm_crc32_u32(crc, pData[i]);
        }
        return crc;
    }
};

}// end SWRL

namespace std
{

template <typename T, int N>
struct hash<SWRL::UncheckedFixedVector<T, N>>
{
	size_t operator() (SWRL::UncheckedFixedVector<T, N> const& v) const
	{
		if (v.size() == 0) return 0;
		std::hash<T> H;
		size_t x = H(v[0]);
		if (v.size() == 1) return x;
		for (size_t i = 1; i < v.size(); ++i)
			x ^= H(v[i]) + 0x9e3779b9 + (x<<6) + (x>>2);
		return x;
	}
};


}// end std.

#endif//SWRLIB_CONTAINERS_HPP__
