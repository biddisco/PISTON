/*
Copyright (c) 2011, Los Alamos National Security, LLC
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation
    	and/or other materials provided with the distribution.
    Neither the name of the Los Alamos National Laboratory nor the names of its contributors may be used to endorse or promote products derived from this
    	software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef IMPLICIT_FUNCTION_H_
#define IMPLICIT_FUNCTION_H_

#include <thrust/tuple.h>
#include <thrust/functional.h>

namespace piston
{
    template <typename IndexType, typename ValueType>
    struct implicit_function2d : public thrust::unary_function<thrust::tuple<IndexType, IndexType>, ValueType>
    {
	typedef thrust::tuple<IndexType, IndexType> InputType;
    };

    template <typename IndexType, typename ValueType>
    struct implicit_function3d : public thrust::unary_function<thrust::tuple<IndexType, IndexType, IndexType>, ValueType>
    {
	typedef thrust::tuple<IndexType, IndexType, IndexType> InputType;
    };
}

#endif /* IMPLICIT_FUNCTION_H_ */
