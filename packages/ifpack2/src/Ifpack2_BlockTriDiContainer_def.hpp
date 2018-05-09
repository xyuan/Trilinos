/*@HEADER
// ***********************************************************************
//
//       Ifpack2: Templated Object-Oriented Algebraic Preconditioner Package
//                 Copyright (2009) Sandia Corporation
//
// Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
// license for use of this work by or on behalf of the U.S. Government.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Michael A. Heroux (maherou@sandia.gov)
//
// ***********************************************************************
//@HEADER
*/

#ifndef IFPACK2_BLOCKTRIDICONTAINER_DEF_HPP
#define IFPACK2_BLOCKTRIDICONTAINER_DEF_HPP

#include <Teuchos_Details_MpiTypeTraits.hpp>

#include <Tpetra_Distributor.hpp>
#include <Tpetra_BlockMultiVector.hpp>

#include <Kokkos_ArithTraits.hpp>
#include <KokkosBatched_Util.hpp>
#include <KokkosBatched_Vector.hpp>
#include <KokkosBatched_AddRadial_Decl.hpp>
#include <KokkosBatched_AddRadial_Impl.hpp>
#include <KokkosBatched_Gemm_Decl.hpp>
#include <KokkosBatched_Gemm_Serial_Impl.hpp>
#include <KokkosBatched_Gemv_Decl.hpp>
#include <KokkosBatched_Gemv_Serial_Impl.hpp>
#include <KokkosBatched_Trsm_Decl.hpp>
#include <KokkosBatched_Trsm_Serial_Impl.hpp>
#include <KokkosBatched_Trsv_Decl.hpp>
#include <KokkosBatched_Trsv_Serial_Impl.hpp>
#include <KokkosBatched_LU_Decl.hpp>
#include <KokkosBatched_LU_Serial_Impl.hpp>

#include "Ifpack2_BlockTriDiContainer_decl.hpp"
#include "Ifpack2_BlockTriDiContainer_impl.hpp"

#include <memory>


namespace Ifpack2 {

  template <typename MatrixType, typename LocalScalarType>
  void
  BlockTriDiContainer<MatrixType, LocalScalarType>::
  initInternal (const Teuchos::RCP<const row_matrix_type>& matrix,
                const Teuchos::Array<Teuchos::Array<local_ordinal_type> >& partitions,
                const Teuchos::RCP<const import_type>& importer,
                const bool overlapCommAndComp,
                const bool useSeqMethod) 
  {
    using impl_type = BlockTriDiContainerDetails::ImplType<MatrixType>;
    using block_crs_matrix_type = typename impl_type::tpetra_block_crs_matrix_type;

    A_ = Teuchos::rcp_dynamic_cast<const block_crs_matrix_type>(matrix);
    TEUCHOS_TEST_FOR_EXCEPT_MSG
      (A_.is_null(), "BlockTriDiContainer currently supports Tpetra::BlockCrsMatrix only.");

    tpetra_importer_ = Teuchos::null;
    async_importer_  = Teuchos::null;
    
    if (importer.is_null()) // there is no given importer, then create one
      if (useSeqMethod) 
        tpetra_importer_ = BlockTriDiContainerDetails::createBlockCrsTpetraImporter<MatrixType>(A_);
      else 
        async_importer_ = BlockTriDiContainerDetails::createBlockCrsAsyncImporter<MatrixType>(A_);
    else 
      tpetra_importer_ = importer; // if there is a given importer, use it

    // as a result, there are 
    // 1) tpetra_importer is     null , async_importer is     null (no need for importer)
    // 2) tpetra_importer is NOT null , async_importer is     null (sequential method is used)
    // 3) tpetra_importer is     null , async_importer is NOT null (async method is used)

    // temporary disabling 
    overlap_communication_and_computation_ = false; //overlapCommAndComp;
    
    Z_ = typename impl_type::tpetra_multivector_type();

    part_interface_  = BlockTriDiContainerDetails::createPartInterface<MatrixType>(A_, partitions);
    block_tridiags_  = BlockTriDiContainerDetails::createBlockTridiags<MatrixType>(part_interface_);
    norm_manager_    = BlockTriDiContainerDetails::NormManager<MatrixType>(A_->getComm());
  }

  template <typename MatrixType, typename LocalScalarType>
  void
  BlockTriDiContainer<MatrixType, LocalScalarType>::
  clearInternal ()
  {
    using impl_type = BlockTriDiContainerDetails::ImplType<MatrixType>;
    using part_interface_type = BlockTriDiContainerDetails::PartInterface<MatrixType>;
    using block_tridiags_type = BlockTriDiContainerDetails::BlockTridiags<MatrixType>;
    using amd_type = BlockTriDiContainerDetails::AmD<MatrixType>;
    using norm_manager_type = BlockTriDiContainerDetails::NormManager<MatrixType>;
    
    A_ = Teuchos::null;
    tpetra_importer_ = Teuchos::null;
    async_importer_  = Teuchos::null;

    Z_ = typename impl_type::tpetra_multivector_type();

    part_interface_  = part_interface_type();
    block_tridiags_  = block_tridiags_type();
    a_minus_d_       = amd_type();
    work_            = typename impl_type::vector_type_1d_view();
    norm_manager_    = norm_manager_type();
  }

  template <typename MatrixType, typename LocalScalarType>
  BlockTriDiContainer<MatrixType, LocalScalarType>::
  BlockTriDiContainer (const Teuchos::RCP<const row_matrix_type>& matrix,
                       const Teuchos::Array<Teuchos::Array<local_ordinal_type> >& partitions,
                       const Teuchos::RCP<const import_type>& importer,
                       int OverlapLevel, scalar_type DampingFactor)
    : Container<MatrixType>(matrix, partitions, importer, OverlapLevel, DampingFactor)
  {
    const bool useSeqMethod = false;
    const bool overlapCommAndComp = false;
    initInternal(matrix, partitions, importer, overlapCommAndComp, useSeqMethod);
  }

  template <typename MatrixType, typename LocalScalarType>
  BlockTriDiContainer<MatrixType, LocalScalarType>::
  BlockTriDiContainer (const Teuchos::RCP<const row_matrix_type>& matrix,
                       const Teuchos::Array<Teuchos::Array<local_ordinal_type> >& partitions,
                       const bool overlapCommAndComp, const bool useSeqMethod)
    : Container<MatrixType>(matrix, partitions, Teuchos::null, 0,
                            Kokkos::ArithTraits<magnitude_type>::one())
  {
    initInternal(matrix, partitions, Teuchos::null, overlapCommAndComp, useSeqMethod);
  }

  template <typename MatrixType, typename LocalScalarType>
  BlockTriDiContainer<MatrixType, LocalScalarType>::~BlockTriDiContainer ()
  {
  }

  template <typename MatrixType, typename LocalScalarType>
  bool BlockTriDiContainer<MatrixType, LocalScalarType>::isInitialized () const
  {
    return IsInitialized_;
  }

  template <typename MatrixType, typename LocalScalarType>
  bool BlockTriDiContainer<MatrixType, LocalScalarType>::isComputed () const
  {
    return IsComputed_;
  }

  template <typename MatrixType, typename LocalScalarType>
  void BlockTriDiContainer<MatrixType, LocalScalarType>::
  setParameters (const Teuchos::ParameterList& /* List */)
  {
    // the solver doesn't currently take any parameters
  }

  template <typename MatrixType, typename LocalScalarType>
  void BlockTriDiContainer<MatrixType, LocalScalarType>::initialize ()
  {
    IsInitialized_ = true;
    // We assume that if you called this method, you intend to recompute
    // everything.
    IsComputed_ = false;
    TEUCHOS_ASSERT(!A_.is_null()); // when initInternal is called, A_ must be set
    {
      BlockTriDiContainerDetails::performSymbolicPhase<MatrixType>
        (A_, part_interface_, block_tridiags_, a_minus_d_, overlap_communication_and_computation_);    
    }
  }

  template <typename MatrixType, typename LocalScalarType>
  void BlockTriDiContainer<MatrixType, LocalScalarType>::compute ()
  {
    IsComputed_ = false;
    if (!this->isInitialized())
      this->initialize();
    {
      BlockTriDiContainerDetails::performNumericPhase<MatrixType>
        (A_, part_interface_, block_tridiags_, Kokkos::ArithTraits<magnitude_type>::zero());
    }
    IsComputed_ = true;
  }

  template <typename MatrixType, typename LocalScalarType>
  void BlockTriDiContainer<MatrixType, LocalScalarType>::clearBlocks ()
  {
    clearInternal();
    IsInitialized_ = false;
    IsComputed_ = false;
  }

  template <typename MatrixType, typename LocalScalarType>
  void BlockTriDiContainer<MatrixType, LocalScalarType>
  ::applyInverseJacobi (const mv_type& X, mv_type& Y, bool zeroStartingSolution,
                        int numSweeps) const
  {
    const magnitude_type tol = Kokkos::ArithTraits<magnitude_type>::zero();
    const int check_tol_every = 1;

    BlockTriDiContainerDetails::applyInverseJacobi<MatrixType>
      (A_,
       tpetra_importer_, 
       async_importer_, 
       overlap_communication_and_computation_,
       X, Y, Z_,
       part_interface_, block_tridiags_, a_minus_d_,
       work_,
       norm_manager_,
       this->DampingFactor_,
       zeroStartingSolution,
       numSweeps,
       tol,
       check_tol_every);
  }

  template <typename MatrixType, typename LocalScalarType>
  BlockTriDiContainer<MatrixType, LocalScalarType>::ComputeParameters::ComputeParameters ()
    : addRadiallyToDiagonal(Kokkos::ArithTraits<magnitude_type>::zero())
  {}

  template <typename MatrixType, typename LocalScalarType>
  typename BlockTriDiContainer<MatrixType, LocalScalarType>::ComputeParameters
  BlockTriDiContainer<MatrixType, LocalScalarType>::createDefaultComputeParameters () const
  {
    return ComputeParameters();
  }

  template <typename MatrixType, typename LocalScalarType>
  void BlockTriDiContainer<MatrixType, LocalScalarType>::compute (const ComputeParameters& in)
  {
    IsComputed_ = false;
    if (!this->isInitialized())
      this->initialize();
    {
      BlockTriDiContainerDetails::performNumericPhase<MatrixType>
        (A_, part_interface_, block_tridiags_, in.addRadiallyToDiagonal);
    }
    IsComputed_ = true;
  }

  template <typename MatrixType, typename LocalScalarType>
  BlockTriDiContainer<MatrixType, LocalScalarType>::ApplyParameters::ApplyParameters ()
    : zeroStartingSolution(false), dampingFactor(Kokkos::ArithTraits<magnitude_type>::one()),
      maxNumSweeps(1), tolerance(Kokkos::ArithTraits<magnitude_type>::zero()),
      checkToleranceEvery(1)
  {}

  template <typename MatrixType, typename LocalScalarType>
  typename BlockTriDiContainer<MatrixType, LocalScalarType>::ApplyParameters
  BlockTriDiContainer<MatrixType, LocalScalarType>::createDefaultApplyParameters () const
  {
    ApplyParameters in;
    in.dampingFactor = this->DampingFactor_;
    return in;
  }

  template <typename MatrixType, typename LocalScalarType>
  int BlockTriDiContainer<MatrixType, LocalScalarType>
  ::applyInverseJacobi (const mv_type& X, mv_type& Y, const ApplyParameters& in) const
  {
    int r_val = 0;
    {
      r_val = BlockTriDiContainerDetails::applyInverseJacobi<MatrixType>
        (A_,
         tpetra_importer_, 
         async_importer_,
         overlap_communication_and_computation_,
         X, Y, Z_,
         part_interface_, block_tridiags_, a_minus_d_,
         work_,
         norm_manager_,
         in.dampingFactor,
         in.zeroStartingSolution,
         in.maxNumSweeps,
         in.tolerance,
         in.checkToleranceEvery);
    }
    return r_val;
  }

  template <typename MatrixType, typename LocalScalarType>
  const Teuchos::ArrayRCP<const typename BlockTriDiContainer<MatrixType, LocalScalarType>::magnitude_type>
  BlockTriDiContainer<MatrixType, LocalScalarType>::getNorms0 () const {
    const auto p = norm_manager_.getNorms0();
    return Teuchos::arcp(p, 0, p ? norm_manager_.getNumVectors() : 0, false);
  }

  template <typename MatrixType, typename LocalScalarType>
  const Teuchos::ArrayRCP<const typename BlockTriDiContainer<MatrixType, LocalScalarType>::magnitude_type>
  BlockTriDiContainer<MatrixType, LocalScalarType>::getNormsFinal () const {
    const auto p = norm_manager_.getNormsFinal();
    return Teuchos::arcp(p, 0, p ? norm_manager_.getNumVectors() : 0, false);
  }

  template <typename MatrixType, typename LocalScalarType>
  void BlockTriDiContainer<MatrixType, LocalScalarType>
  ::apply (HostView& X, HostView& Y, int blockIndex, int stride, Teuchos::ETransp mode,
           scalar_type alpha, scalar_type beta) const
  {
    TEUCHOS_TEST_FOR_EXCEPT_MSG(
                                true, "BlockTriDiContainer::apply is not implemented. You may have reached this message "
                                << "because you want to use this container's performance-portable Jacobi iteration. In "
                                << "that case, set \"relaxation: type\" to \"MT Split Jacobi\" rather than \"Jacobi\".");
  }

  template <typename MatrixType, typename LocalScalarType>
  void BlockTriDiContainer<MatrixType, LocalScalarType>
  ::weightedApply (HostView& X, HostView& Y, HostView& D, int blockIndex, int stride,
                   Teuchos::ETransp mode, scalar_type alpha, scalar_type beta) const
  {
    TEUCHOS_TEST_FOR_EXCEPT_MSG(true, "BlockTriDiContainer::weightedApply is not implemented.");
  }

  template <typename MatrixType, typename LocalScalarType>
  std::ostream& BlockTriDiContainer<MatrixType, LocalScalarType>::print (std::ostream& os) const
  {
    Teuchos::FancyOStream fos(Teuchos::rcp(&os,false));
    fos.setOutputToRootOnly(0);
    describe(fos);
    return os;
  }

  template <typename MatrixType, typename LocalScalarType>
  std::string BlockTriDiContainer<MatrixType, LocalScalarType>::description () const
  {
    std::ostringstream oss;
    oss << Teuchos::Describable::description();
    if (isInitialized()) {
      if (isComputed()) {
        oss << "{status = initialized, computed";
      }
      else {
        oss << "{status = initialized, not computed";
      }
    }
    else {
      oss << "{status = not initialized, not computed";
    }

    oss << "}";
    return oss.str();
  }

  template <typename MatrixType, typename LocalScalarType>
  void
  BlockTriDiContainer<MatrixType, LocalScalarType>::
  describe (Teuchos::FancyOStream& os,
            const Teuchos::EVerbosityLevel verbLevel) const
  {
    using std::endl;
    if(verbLevel==Teuchos::VERB_NONE) return;
    os << "================================================================================" << endl
       << "Ifpack2::BlockTriDiContainer" << endl
       << "Number of blocks        = " << this->numBlocks_ << endl
       << "isInitialized()         = " << IsInitialized_ << endl
       << "isComputed()            = " << IsComputed_ << endl
      //<< "impl                    = " << impl_->describe() << endl
       << "================================================================================" << endl
       << endl;
  }

  template <typename MatrixType, typename LocalScalarType>
  std::string BlockTriDiContainer<MatrixType, LocalScalarType>::getName() { return "BlockTriDi"; }

#define IFPACK2_BLOCKTRIDICONTAINER_INSTANT(S,LO,GO,N)                  \
  template class Ifpack2::BlockTriDiContainer< Tpetra::RowMatrix<S, LO, GO, N>, S >;
}
#endif
