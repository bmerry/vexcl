#ifndef VEXCL_SPMAT_CCSR_HPP
#define VEXCL_SPMAT_CCSR_HPP

/*
The MIT License

Copyright (c) 2012-2013 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   vexcl/spmat/ccsr.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  Sparse matrix in CCSR format.
 */

#include <vexcl/vector.hpp>

namespace vex {

/// Sparse matrix in CCSR format.
/**
 * Compressed CSR format. row, col, and val arrays contain unique rows of the
 * matrix. Column numbers in col array are relative to diagonal. idx array
 * contains index into row vector, corresponding to each row of the matrix. So
 * that matrix-vector multiplication may be performed as follows:
 * \code
 * for(uint i = 0; i < n; i++) {
 *     real sum = 0;
 *     for(uint j = row[idx[i]]; j < row[idx[i] + 1]; j++)
 *         sum += val[j] * x[i + col[j]];
 *     y[i] = sum;
 * }
 * \endcode
 * This format does not support multi-device computation, so it accepts single
 * queue at initialization. Vectors x and y should also be single-queued and
 * reside on the same device with matrix.
 */
template <typename real, typename col_t = ptrdiff_t, typename idx_t = size_t>
struct SpMatCCSR {
    static_assert(std::is_signed<col_t>::value,
            "Column type for CCSR format has to be signed.");

    /// Constructor for CCSR format.
    /**
     * Constructs GPU representation of the CCSR matrix.
     * \param queue single queue.
     * \param n     number of rows in the matrix.
     * \param m     number of unique rows in the matrix.
     * \param idx   index into row vector.
     * \param row   row index into col and val vectors.
     * \param col   column positions of nonzero elements wrt to diagonal.
     * \param val   values of nonzero elements of the matrix.
     */
    SpMatCCSR(const cl::CommandQueue &queue, size_t n, size_t m,
            const idx_t *idx, const idx_t *row, const col_t *col, const real *val
            )
        : queue(queue), n(n),
          idx(qctx(queue), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(idx_t) * n,      const_cast<idx_t*>(idx)),
          row(qctx(queue), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(idx_t) * (m+1),  const_cast<idx_t*>(row)),
          col(qctx(queue), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(col_t) * row[m], const_cast<col_t*>(col)),
          val(qctx(queue), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(real)  * row[m], const_cast<real*> (val))
    { }

    cl::CommandQueue queue;
    size_t n;
    cl::Buffer idx;
    cl::Buffer row;
    cl::Buffer col;
    cl::Buffer val;
};

/// \cond INTERNAL
struct ccsr_product_terminal {};

typedef vector_expression<
    typename boost::proto::terminal< ccsr_product_terminal >::type
    > ccsr_product_terminal_expression;

template <typename real, typename col_t, typename idx_t, typename T>
struct ccsr_product : public ccsr_product_terminal_expression
{
    typedef SpMatCCSR<real, col_t, idx_t> matrix;

    const matrix    &A;
    const vector<T> &x;

    ccsr_product(const matrix &A, const vector<T> &x) : A(A), x(x) {}
};

template <typename real, typename col_t, typename idx_t, typename T>
ccsr_product<real, col_t, idx_t, T> operator*(
        const SpMatCCSR<real, col_t, idx_t> &A,
        const vector<T> &x)
{
    return ccsr_product<real, col_t, idx_t, T>(A, x);
}


// Allow ccsr_product to participate in vector expressions:
template <>
struct is_vector_expr_terminal< ccsr_product_terminal >
    : std::true_type
{ };

template <typename real, typename col_t, typename idx_t, typename T>
struct kernel_name< ccsr_product<real, col_t, idx_t, T> > {
    static std::string get() {
        return "spmv_";
    }
};

template <typename real, typename col_t, typename idx_t, typename T>
struct partial_vector_expr< ccsr_product<real, col_t, idx_t, T> > {
    static std::string get(int component, int position, kernel_generator_state&) {
        std::ostringstream prm;
        prm << "prm_" << component << "_" << position << "_";

        std::ostringstream s;
        s << "spmv_" << component << "_" << position << "("
          << prm.str() << "idx, "
          << prm.str() << "row, "
          << prm.str() << "col, "
          << prm.str() << "val, "
          << prm.str() << "vec, idx)";

        return s.str();
    }
};

template <typename real, typename col_t, typename idx_t, typename T>
struct terminal_preamble< ccsr_product<real, col_t, idx_t, T> > {
    static std::string get(int component, int position, kernel_generator_state&) {
        std::ostringstream s;

        typedef decltype(real() * T()) res_t;

        s << type_name<res_t>() <<
          " spmv_" << component << "_" << position << "(\n"
          "\tglobal " << type_name<idx_t>() << " * idx,\n"
          "\tglobal " << type_name<idx_t>() << " * row,\n"
          "\tglobal " << type_name<col_t>() << " * col,\n"
          "\tglobal " << type_name<real>()  << " * val,\n"
          "\tglobal " << type_name<T>()     << " * vec,\n"
          "\tulong i\n\t)\n{\n"
          "\t" << type_name<res_t>() << " sum = 0;\n"
          "\tfor(size_t pos = idx[i], j = row[pos], end = row[pos+1]; j < end; ++j)\n"
          "\t\tsum += val[j] * vec[i + col[j]];\n"
          "\treturn sum;\n"
          "}\n";
        return s.str();
    }
};

template <typename real, typename col_t, typename idx_t, typename T>
struct kernel_param_declaration< ccsr_product<real, col_t, idx_t, T> > {
    static std::string get(int component, int position, kernel_generator_state&) {
        std::ostringstream prm;
        prm << "prm_" << component << "_" << position << "_";

        std::ostringstream s;
        s << ",\n\tglobal " << type_name<idx_t>() << " * " << prm.str() << "idx"
          << ",\n\tglobal " << type_name<idx_t>() << " * " << prm.str() << "row"
          << ",\n\tglobal " << type_name<col_t>() << " * " << prm.str() << "col"
          << ",\n\tglobal " << type_name<real>()  << " * " << prm.str() << "val"
          << ",\n\tglobal " << type_name<T>()     << " * " << prm.str() << "vec";

        return s.str();
    }
};

template <typename real, typename col_t, typename idx_t, typename T>
struct kernel_arg_setter< ccsr_product<real, col_t, idx_t, T> > {
    static void set(cl::Kernel &kernel, uint device, size_t/*index_offset*/,
            uint &position, const ccsr_product<real, col_t, idx_t, T> &term, kernel_generator_state&)
    {
        assert(device == 0);

        kernel.setArg(position++, term.A.idx);
        kernel.setArg(position++, term.A.row);
        kernel.setArg(position++, term.A.col);
        kernel.setArg(position++, term.A.val);
        kernel.setArg(position++, term.x(device));
    }
};

template <typename real, typename col_t, typename idx_t, typename T>
struct expression_properties< ccsr_product<real, col_t, idx_t, T> > {
    static void get(const ccsr_product<real, col_t, idx_t, T> &term,
            std::vector<cl::CommandQueue> &queue_list,
            std::vector<size_t> &partition,
            size_t &size
            )
    {
        queue_list = term.x.queue_list();
        partition  = term.x.partition();
        size       = term.x.size();

        assert(partition.size() == 2);
        partition.back() = size;
    }
};

/// \endcond

} // namespace vex

#endif