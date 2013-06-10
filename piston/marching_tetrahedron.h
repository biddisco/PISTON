/*
Copyright (c) 2011, Los Alamos National Security, LLC
All rights reserved.
Copyright 2011. Los Alamos National Security, LLC. This software was produced under U.S. Government contract DE-AC52-06NA25396 for Los Alamos National Laboratory (LANL),
which is operated by Los Alamos National Security, LLC for the U.S. Department of Energy. The U.S. Government has rights to use, reproduce, and distribute this software.

NEITHER THE GOVERNMENT NOR LOS ALAMOS NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.

If software is modified to produce derivative works, such modified software should be clearly marked, so as not to confuse it with the version available from LANL.

Additionally, redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
·         Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
·         Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other
          materials provided with the distribution.
·         Neither the name of Los Alamos National Security, LLC, Los Alamos National Laboratory, LANL, the U.S. Government, nor the names of its contributors may be used
          to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY LOS ALAMOS NATIONAL SECURITY, LLC AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL LOS ALAMOS NATIONAL SECURITY, LLC OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef MARCHING_TETRAHEDRON_H_
#define MARCHING_TETRAHEDRON_H_

#include <thrust/copy.h>
#include <thrust/scan.h>
#include <thrust/transform_scan.h>
#include <thrust/binary_search.h>
#include <thrust/iterator/constant_iterator.h>
#include <thrust/iterator/zip_iterator.h>

#include <piston/image3d.h>
#include <piston/piston_math.h>
#include <piston/choose_container.h>

namespace piston
{

template <typename InputDataSet1, typename InputDataSet2 = InputDataSet1>
struct marching_tetrahedron
{
public:
    typedef typename InputDataSet1::PointDataIterator InputPointDataIterator;
    typedef typename InputDataSet1::GridCoordinatesIterator InputGridCoordinatesIterator;
    typedef typename InputDataSet1::PhysicalCoordinatesIterator InputPhysCoordinatesIterator;
    typedef typename InputDataSet2::PointDataIterator ScalarSourceIterator;

    typedef typename thrust::iterator_difference<InputPointDataIterator>::type	diff_type;
    typedef typename thrust::iterator_space<InputPointDataIterator>::type	space_type;
    typedef typename thrust::iterator_value<InputPointDataIterator>::type	value_type;

    typedef typename thrust::counting_iterator<int, space_type>	CountingIterator;

    typedef typename detail::choose_container<InputPointDataIterator, int>::type  TableContainer;
    typedef typename detail::choose_container<InputPointDataIterator, int>::type  IndicesContainer;

    typedef typename detail::choose_container<InputPointDataIterator, float4>::type 	VerticesContainer;
    typedef typename detail::choose_container<InputPointDataIterator, float3>::type	NormalsContainer;
    typedef typename detail::choose_container<ScalarSourceIterator, float>::type	ScalarContainer;

    typedef typename TableContainer::iterator	 TableIterator;
    typedef typename VerticesContainer::iterator VerticesIterator;
    typedef typename IndicesContainer::iterator  IndicesIterator;
    typedef typename NormalsContainer::iterator  NormalsIterator;
    typedef typename ScalarContainer::iterator   ScalarIterator;


    static const int triTable_array[16][7];
    static const int numVerticesTable_array[16];

    InputDataSet1 &input;		// scalar field for generating isosurface/cut geometry
    InputDataSet2 &source;		// scalar field for generating interpolated scalar values

    value_type isovalue;
    bool useInterop;

    TableContainer	triTable;	// a copy of triangle edge indices table in host|device_vector
    TableContainer	numVertsTable;	// a copy of number of vertices per cell table in host|device_vector

    IndicesContainer	case_index;	// classification of cells as indices into triTable and numVertsTable
    IndicesContainer	num_vertices;	// number of vertices will be generated by the cell

    IndicesContainer 	valid_cell_enum;	// enumeration of valid cells
    IndicesContainer	valid_cell_indices;	// a sequence of indices to valid cells

    IndicesContainer 	output_vertices_enum;	// enumeration of output vertices, only valid ones

#ifdef USE_INTEROP
    value_type minIso, maxIso;
    bool colorFlip;
    float4 *vertexBufferData;
    float3 *normalBufferData;
    float4 *colorBufferData;
    int vboSize;
    GLuint vboBuffers[3];
    struct cudaGraphicsResource* vboResources[3]; // vertex buffers for interop
#endif

    VerticesContainer	vertices; 	// output vertices, only valid ones
    NormalsContainer	normals;	// surface normal computed by cross product of triangle edges
    ScalarContainer	scalars;	// interpolated scalar output

    unsigned int num_total_vertices;

    marching_tetrahedron(InputDataSet1 &input,  InputDataSet2 &source,
                         value_type isovalue = value_type()) :
	input(input), source(source), isovalue(isovalue),
	triTable((int*) triTable_array, (int*) triTable_array+16*7),
	numVertsTable((int *) numVerticesTable_array, (int *) numVerticesTable_array+16)
#ifdef USE_INTEROP
    , colorFlip(false), vboSize(0), useInterop(false)
#endif
      {}

    void operator()()
    {
	const int NCells = input.NCells;

	case_index.resize(NCells);
	num_vertices.resize(NCells);

	// classify all cells, generate indices into triTable and numVertsTable,
	// we also use numVertsTable to generate num_vertices for each cell
	thrust::transform(CountingIterator(0), CountingIterator(0)+NCells,
	                  thrust::make_zip_iterator(thrust::make_tuple(case_index.begin(), num_vertices.begin())),
	                  classify_cell(input, isovalue,
	                                numVertsTable.begin()));

	// enumerate valid cells
	valid_cell_enum.resize(NCells);
	thrust::transform_inclusive_scan(num_vertices.begin(), num_vertices.end(),
	                                 valid_cell_enum.begin(),
	                                 is_valid_cell(),
	                                 thrust::plus<int>());
	// the total number of valid cells is the last element of the enumeration.
	unsigned int num_valid_cells = valid_cell_enum.back();

	// no valid cells at all, return with empty vectors.
	if (num_valid_cells == 0) {
	    vertices.clear();
	    normals.clear();
	    scalars.clear();
	    return;
	}

	// find indices to valid cells
	valid_cell_indices.resize(num_valid_cells);
	thrust::upper_bound(valid_cell_enum.begin(), valid_cell_enum.end(),
	                    CountingIterator(0), CountingIterator(0)+num_valid_cells,
	                    valid_cell_indices.begin());

	// use indices to valid cells to fetch number of vertices generated by
	// valid cells and do an enumeration to get the output indices for
	// the first vertex generated by the valid cells.
	output_vertices_enum.resize(num_valid_cells);
	thrust::exclusive_scan(thrust::make_permutation_iterator(num_vertices.begin(), valid_cell_indices.begin()),
	                       thrust::make_permutation_iterator(num_vertices.begin(), valid_cell_indices.begin()) + num_valid_cells,
	                       output_vertices_enum.begin());

	// get the total number of vertices,
	num_total_vertices = num_vertices[valid_cell_indices.back()] + output_vertices_enum.back();

	if (useInterop)
	{
#if USE_INTEROP
	    if (num_total_vertices > vboSize)
	    {
              glBindBuffer(GL_ARRAY_BUFFER, vboBuffers[0]);
              glBufferData(GL_ARRAY_BUFFER, num_total_vertices*sizeof(float4), 0, GL_DYNAMIC_DRAW);
              if (glGetError() == GL_OUT_OF_MEMORY) { std::cout << "Out of VBO memory" << std::endl; exit(-1); }
              glBindBuffer(GL_ARRAY_BUFFER, vboBuffers[1]);
              glBufferData(GL_ARRAY_BUFFER, num_total_vertices*sizeof(float4), 0, GL_DYNAMIC_DRAW);
              if (glGetError() == GL_OUT_OF_MEMORY) { std::cout << "Out of VBO memory" << std::endl; exit(-1); }
              glBindBuffer(GL_ARRAY_BUFFER, vboBuffers[2]);
              glBufferData(GL_ARRAY_BUFFER, num_total_vertices*sizeof(float3), 0, GL_DYNAMIC_DRAW);
              if (glGetError() == GL_OUT_OF_MEMORY) { std::cout << "Out of VBO memory" << std::endl; exit(-1); }
              glBindBuffer(GL_ARRAY_BUFFER, 0);
              vboSize = num_total_vertices;
	    }
	    size_t num_bytes;
	    cudaGraphicsMapResources(1, &vboResources[0], 0);
	    cudaGraphicsResourceGetMappedPointer((void **) &vertexBufferData,
						 &num_bytes, vboResources[0]);

	    if (vboResources[1]) {
		cudaGraphicsMapResources(1, &vboResources[1], 0);
		cudaGraphicsResourceGetMappedPointer((void **) &colorBufferData,
						     &num_bytes, vboResources[1]);
	    }

	    cudaGraphicsMapResources(1, &vboResources[2], 0);
	    cudaGraphicsResourceGetMappedPointer((void **) &normalBufferData,
						 &num_bytes, vboResources[2]);
#endif
	} else
	{
	    vertices.resize(num_total_vertices);
	    normals.resize(num_total_vertices);
	}

	// resize vectors according to the total number of vertices generated.
	//vertices.resize(num_total_vertices);
	//normals.resize(num_total_vertices);
	scalars.resize(num_total_vertices);

	// do edge interpolation for each valid cell
	if (useInterop)
	{
#if USE_INTEROP
	    thrust::for_each(thrust::make_zip_iterator(thrust::make_tuple(valid_cell_indices.begin(), output_vertices_enum.begin(),
	                                                                  thrust::make_permutation_iterator(case_index.begin(), valid_cell_indices.begin()),
	                                                                  thrust::make_permutation_iterator(num_vertices.begin(), valid_cell_indices.begin()))),
	                     thrust::make_zip_iterator(thrust::make_tuple(valid_cell_indices.end(), output_vertices_enum.end(),
	                                                                  thrust::make_permutation_iterator(case_index.begin(), valid_cell_indices.begin()) + num_valid_cells,
	                                                                  thrust::make_permutation_iterator(num_vertices.begin(), valid_cell_indices.begin()) + num_valid_cells)),
	                     isosurface_functor(input, source, isovalue, 
	                                        triTable.begin(),
	                                        vertexBufferData,
	                                        normalBufferData,
	                                        thrust::raw_pointer_cast(&*scalars.begin())));
	    if (vboResources[1])
		thrust::transform(scalars.begin(), scalars.end(),
		                  thrust::device_ptr<float4>(colorBufferData),
		                  color_map<float>(minIso, maxIso, colorFlip));
	    for (int i = 0; i < 3; i++)
		cudaGraphicsUnmapResources(1, &vboResources[i], 0);
#endif
	}
	else
	{
	    thrust::for_each(thrust::make_zip_iterator(thrust::make_tuple(valid_cell_indices.begin(), output_vertices_enum.begin(),
	                                                                  thrust::make_permutation_iterator(case_index.begin(),   valid_cell_indices.begin()),
	                                                                  thrust::make_permutation_iterator(num_vertices.begin(), valid_cell_indices.begin()))),
	                     thrust::make_zip_iterator(thrust::make_tuple(valid_cell_indices.end(),   output_vertices_enum.end(),
	                                                                  thrust::make_permutation_iterator(case_index.begin(),   valid_cell_indices.begin()) + num_valid_cells,
	                                                                  thrust::make_permutation_iterator(num_vertices.begin(), valid_cell_indices.begin()) + num_valid_cells)),
	                     isosurface_functor(input,
	                                        source,
	                                        isovalue,
	                                        triTable.begin(),
	                                        thrust::raw_pointer_cast(&*vertices.begin()),
	                                        thrust::raw_pointer_cast(&*normals.begin()),
	                                        thrust::raw_pointer_cast(&*scalars.begin())));
	}
    }

    struct classify_cell : public thrust::unary_function<int, thrust::tuple<int, int> >
    {
	// FixME: constant iterator and/or iterator to const problem.
	InputPointDataIterator	point_data;
	const float 		isovalue;
	TableIterator		numVertsTable;

	classify_cell(InputDataSet1 &input, float isovalue,
	              TableIterator numVertsTable) :
	    point_data(input.point_data_begin()),
	    isovalue(isovalue),
	    numVertsTable(numVertsTable) {}

	__host__ __device__
	thrust::tuple<int, int> operator() (int cell_id) const {
	    const float f0 = *(point_data + cell_id*4);
	    const float f1 = *(point_data + cell_id*4 + 1);
	    const float f2 = *(point_data + cell_id*4 + 2);
	    const float f3 = *(point_data + cell_id*4 + 3);

	    unsigned int case_num = (f0 < isovalue);
	    case_num += (f1 < isovalue)*2;
	    case_num += (f2 < isovalue)*4;
	    case_num += (f3 < isovalue)*8;

	    return thrust::make_tuple(case_num, numVertsTable[case_num]);
	}
    };

    struct is_valid_cell : public thrust::unary_function<int, int>
    {
	__host__ __device__
	int operator()(int num_vertices) const {
	    return num_vertices != 0;
	}
    };

    struct isosurface_functor : public thrust::unary_function<thrust::tuple<int, int, int, int>, void>
    {
	// FixME: constant iterator and/or iterator to const problem.
	InputPointDataIterator	point_data;
	InputPhysCoordinatesIterator physical_coord;
	ScalarSourceIterator	scalar_source;
	const float		isovalue;
	TableIterator		triangle_table;

	typedef typename InputPhysCoordinatesIterator::value_type	grid_tuple_type;

	float4 *vertices_output;
	float3 *normals_output;
	float  *scalars_output;

	__host__ __device__
	isosurface_functor(InputDataSet1 &input,
	                   InputDataSet2 &source,
	                   const float isovalue,
	                   TableIterator triangle_table,
	                   float4 *vertices,
	                   float3 *normals,
	                   float  *scalars)
	    : point_data(input.point_data_begin()),
	      physical_coord(input.physical_coordinates_begin()),
	      scalar_source(source.point_data_begin()),
	      isovalue(isovalue), 
	      triangle_table(triangle_table),
	      vertices_output(vertices),
	      normals_output(normals),
	      scalars_output(scalars)
	{}

	__host__ __device__
	float3 vertex_interp(float3 p0, float3 p1, float t) const {
	    return lerp(p0, p1, t);
	}
	__host__ __device__
	float scalar_interp(float s0, float s1, float t) const {
	    return lerp(s0, s1, t);
	}
	__host__ __device__
	float3 tuple2float3(thrust::tuple<int, int, int> xyz) {
	    return make_float3((float) thrust::get<0>(xyz),
	                       (float) thrust::get<1>(xyz),
	                       (float) thrust::get<2>(xyz));
	}

	__host__ __device__
	void operator()(thrust::tuple<int, int, int, int> indices_tuple) {
	    const int cell_id  	   = thrust::get<0>(indices_tuple);
	    const int outputVertId = thrust::get<1>(indices_tuple);
	    const int cubeindex    = thrust::get<2>(indices_tuple);
	    const int numVertices  = thrust::get<3>(indices_tuple);

	    const int verticesForEdge[] =
	    {
		 0, 1, // edge 0 : vertex 0 -> vertex 1
		 1, 2, // edge 1 : vertex 1 -> vertex 2
		 0, 2, // edge 2 : vertex 0 -> vertex 2
		 0, 3, // edge 3 : vertex 0 -> vertex 3
		 1, 3, // edge 4 : vertex 1 -> vertex 3
		 2, 3  // edge 5 : vertex 2 -> vertex 3
	    };

	    float f[4];
	    f[0] = *(point_data + cell_id*4);
	    f[1] = *(point_data + cell_id*4 + 1);
	    f[2] = *(point_data + cell_id*4 + 2);
	    f[3] = *(point_data + cell_id*4 + 3);


	    // TODO: Reconsider what GridCoordinates should be (tuple or float3)
	    float3 p[4];
	    p[0] = tuple2float3(*(physical_coord + cell_id*4));
	    p[1] = tuple2float3(*(physical_coord + cell_id*4 + 1));
	    p[2] = tuple2float3(*(physical_coord + cell_id*4 + 2));
	    p[3] = tuple2float3(*(physical_coord + cell_id*4 + 3));

	    float s[8];
	    s[0] = *(scalar_source + cell_id*4);
	    s[1] = *(scalar_source + cell_id*4 + 1);
	    s[2] = *(scalar_source + cell_id*4 + 2);
	    s[3] = *(scalar_source + cell_id*4 + 3);

	    // interpolation for vertex positions and associated scalar values
	    for (int v = 0; v < numVertices; v++) {
		const int edge = triangle_table[cubeindex*7 + v];
		const int v0   = verticesForEdge[2*edge];
		const int v1   = verticesForEdge[2*edge + 1];
		const float t  = (isovalue - f[v0]) / (f[v1] - f[v0]);
		*(vertices_output + outputVertId + v) = make_float4(vertex_interp(p[v0], p[v1], t), 1.0f);
		*(scalars_output  + outputVertId + v) = scalar_interp(s[v0], s[v1], t);
	    }

	    // generate normal vectors by cross product of triangle edges
	    for (int v = 0; v < numVertices; v += 3) {
		const float4 *vertex = (vertices_output + outputVertId + v);
		const float3 edge0 = make_float3(vertex[1] - vertex[0]);
		const float3 edge1 = make_float3(vertex[2] - vertex[0]);
		const float3 normal = normalize(cross(edge0, edge1));
		*(normals_output + outputVertId + v) =
		*(normals_output + outputVertId + v + 1) =
		*(normals_output + outputVertId + v + 2) = normal;
	    }
	}
    };

    VerticesIterator vertices_begin() {
	return vertices.begin();
    }
    VerticesIterator vertices_end() {
	return vertices.end();
    }

    NormalsIterator normals_begin() {
	return normals.begin();
    }
    NormalsIterator normals_end() {
	return normals.end();
    }

    ScalarIterator scalars_begin() {
	return scalars.begin();
    }
    ScalarIterator scalars_end() {
	return scalars.end();
    }

    void set_isovalue(value_type val) {
	isovalue = val;
    }
};

template <typename InputDataSet1, typename InputDataSet2>
const int marching_tetrahedron<InputDataSet1, InputDataSet2>::triTable_array[16][7] =
{
     {-1, -1, -1, -1, -1, -1, -1},
     { 0,  3,  2, -1, -1, -1, -1},
     { 0,  1,  4, -1, -1, -1, -1},
     { 1,  4,  2,  2,  4,  3, -1},

     { 1,  2,  5, -1, -1, -1, -1},
     { 0,  3,  5,  0,  5,  1, -1},
     { 0,  2,  5,  0,  5,  4, -1},
     { 5,  4,  3, -1, -1, -1, -1},

     { 3,  4,  5, -1, -1, -1, -1},
     { 4,  5,  0,  5,  2,  0, -1},
     { 1,  5,  0,  5,  3,  0, -1},
     { 5,  2,  1, -1, -1, -1, -1},

     { 3,  4,  2,  2,  4,  1, -1},
     { 4,  1,  0, -1, -1, -1, -1},
     { 2,  3,  0, -1, -1, -1, -1},
     {-1, -1, -1, -1, -1, -1, -1},
};

template <typename InputDataSet1, typename InputDataSet2>
const int marching_tetrahedron<InputDataSet1, InputDataSet2>::numVerticesTable_array [16] =
{
    0, 3, 3, 6, 3, 6, 6, 3,
    3, 6, 6, 3, 6, 3, 3, 0
};

}

#endif /* MARCHING_TETRAHEDRON_H_ */
