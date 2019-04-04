//                                Remhos Remap Mini-App
//
// Compile with: make
//
// Sample runs:
//    Using lua problem definition file
//    ./remhos -p balls-and-jacks.lua -r 4 -dt 0.001 -tf 5.0
//
//    Transport mode:
//    ./remhos -m ./data/periodic-segment.mesh -p 0 -r 2 -dt 0.005
//    ./remhos -m ./data/periodic-square.mesh -p 0 -r 2 -dt 0.01 -tf 10
//    ./remhos -m ./data/periodic-hexagon.mesh -p 0 -r 2 -dt 0.01 -tf 10
//    ./remhos -m ./data/periodic-square.mesh -p 1 -r 2 -dt 0.005 -tf 9
//    ./remhos -m ./data/periodic-hexagon.mesh -p 1 -r 2 -dt 0.005 -tf 9
//    ./remhos -m ./data/star-q3.mesh -p 1 -r 2 -dt 0.005 -tf 9
//    ./remhos -m ./data/disc-nurbs.mesh -p 1 -r 3 -dt 0.005 -tf 9
//    ./remhos -m ./data/disc-nurbs.mesh -p 2 -r 3 -dt 0.005 -tf 9
//    ./remhos -m ./data/periodic-square.mesh -p 3 -r 4 -dt 0.0025 -tf 9 -vs 20
//    ./remhos -m ./data/periodic-cube.mesh -p 0 -r 2 -o 2 -dt 0.02 -tf 8
//    ./remhos -m ./data/periodic-square.mesh -p 4 -r 4 -dt 0.001 -o 2 -mt 3
//    ./remhos -m ./data/periodic-square.mesh -p 3 -r 2 -dt 0.0025 -o 15 -tf 9 -mt 4
//    ./remhos -m ./data/periodic-square.mesh -p 5 -r 4 -dt 0.002 -o 2 -tf 0.8 -mt 4
//    ./remhos -m ./data/periodic-cube.mesh -p 5 -r 5 -dt 0.0001 -o 1 -tf 0.8 -mt 4
//
//    Remap mode:
//    ./remhos -m ./data/periodic-square.mesh -p 10 -r 3 -dt 0.005 -tf 0.5 -mt 4 -vs 10
//    ./remhos -m ./data/periodic-square.mesh -p 14 -r 3 -dt 0.005 -tf 0.5 -mt 4 -vs 10
//
//
// Description:  This example code solves the time-dependent advection equation
//               du/dt + v.grad(u) = 0, where v is a given fluid velocity, and
//               u0(x)=u(0,x) is a given initial condition.
//
//               The example demonstrates the use of Discontinuous Galerkin (DG)
//               bilinear forms in MFEM (face integrators), the use of explicit
//               ODE time integrators, the definition of periodic boundary
//               conditions through periodic meshes, as well as the use of GLVis
//               for persistent visualization of a time-evolving solution. The
//               saving of time-dependent data files for external visualization
//               with VisIt (visit.llnl.gov) is also illustrated.
#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>

using namespace std;
using namespace mfem;

#ifdef USE_LUA
#include "lua.hpp"
lua_State* L;
#endif

// Choice for the problem setup. The fluid velocity, initial condition and
// inflow boundary condition are chosen based on this parameter.
int problem_num;

// 0 is standard transport.
// 1 is standard remap (mesh moves, solution is fixed).
int exec_mode;

// Velocity coefficient
void velocity_function(const Vector &x, Vector &v);

// Initial condition
double u0_function(const Vector &x);

// Inflow boundary condition
double inflow_function(const Vector &x);

// Mesh bounding box
Vector bb_min, bb_max;

enum MONOTYPE { None, DiscUpw, DiscUpw_FCT, ResDist, ResDist_FCT };

struct LowOrderMethod
{
   MONOTYPE MonoType;
   bool OptScheme;
   DG_FECollection *fec0, *fec1;
   FiniteElementSpace *fes, *SubFes0, *SubFes1;
   Array <int> smap;
   SparseMatrix D;
   BilinearForm* pk;
   VectorCoefficient* coef;
   const IntegrationRule* irF;
   BilinearFormIntegrator* VolumeTerms;
   Mesh* subcell_mesh;
};

// Utility function to build a map to the offset
// of the symmetric entry in a sparse matrix.
Array<int> SparseMatrix_Build_smap(const SparseMatrix &A)
{
   // Assuming that A is finalized
   const int *I = A.GetI(), *J = A.GetJ(), n = A.Size();
   Array<int> smap;
   smap.SetSize(I[n]);

   for (int row = 0, j = 0; row < n; row++)
   {
      for (int end = I[row+1]; j < end; j++)
      {
         int col = J[j];
         // Find the offset, _j, of the (col,row) entry
         // and store it in smap[j].
         for (int _j = I[col], _end = I[col+1]; true; _j++)
         {
            if (_j == _end)
            {
               mfem_error("SparseMatrix_Build_smap");
            }

            if (J[_j] == row)
            {
               smap[j] = _j;
               break;
            }
         }
      }
   }
   return smap;
}

// Given a matrix K, matrix D (initialized with same sparsity as K) 
// is computed, such that (K+D)_ij >= 0 for i != j.
void ComputeDiscreteUpwindingMatrix(const SparseMatrix& K,
                                    Array<int> smap, SparseMatrix& D)
{
   const int n = K.Size();
   int* Ip = K.GetI();
   int* Jp = K.GetJ();
   double* Kp = K.GetData();

   double* Dp = D.GetData();

   for (int i = 0, k = 0; i < n; i++)
   {
      double rowsum = 0.;
      for (int end = Ip[i+1]; k < end; k++)
      {
         int j = Jp[k];
         double kij = Kp[k];
         double kji = Kp[smap[k]];
         double dij = fmax(fmax(0.0,-kij),-kji);
         Dp[k] = kij + dij;
         Dp[smap[k]] = kji + dij;
         if (i != j) { rowsum += dij; }
      }
      D(i,i) = K(i,i) -rowsum;
   }
}

// The mesh corresponding to Bezier subcells of order p is constructed.
// NOTE: The mesh is assumed to consist of segments, quads or hexes.
// TODO It seems that for an original mesh with periodic boundaries, this
// routine produces a mesh that cannot be used for its intended purpose.
Mesh* GetSubcellMesh(Mesh *mesh, int p)
{
   Mesh *subcell_mesh;
   if (p == 1) // This case should never be called
   {
      subcell_mesh = mesh;
   }
   else if (mesh->Dimension() > 1)
   {
      int basis_lor = BasisType::ClosedUniform; // Get a uniformly refined mesh.
      subcell_mesh = new Mesh(mesh, p, basis_lor);
      // NOTE: Curvature is not considered for subcell weights.
      subcell_mesh->SetCurvature(1);
   }
   else
   {
      // TODO generalize to arbitrary 1D segments (different length than 1).
      subcell_mesh = new Mesh(mesh->GetNE()*p, 1.);
      subcell_mesh->SetCurvature(1);
   }
   return subcell_mesh;
}

// Appropriate quadrature rule for faces of is obtained.
// TODO check if this gives the desired order. I use the same order
// for all faces. In DGTraceIntegrator it uses the min of OrderW, why?
const IntegrationRule *GetFaceIntRule(FiniteElementSpace *fes)
{
   int i, qOrdF;
   Mesh* mesh = fes->GetMesh();
   FaceElementTransformations *Trans;
   
   // Use the first mesh face with two elements as indicator.
   for (i = 0; i < mesh->GetNumFaces(); i++)
   {
      Trans = mesh->GetFaceElementTransformations(i);
      qOrdF = Trans->Elem1->OrderW();
      if (Trans->Elem2No >= 0)
      {
         // qOrdF is chosen such that L2-norm of basis functions is
         // computed accurately.
         qOrdF = max(qOrdF, Trans->Elem2->OrderW());
         break;
      }
   }
   // Use the first mesh element as indicator.
   const FiniteElement &dummy = *fes->GetFE(0);
   qOrdF += 2*dummy.GetOrder();

   return &IntRules.Get(Trans->FaceGeom, qOrdF);
}

// Class storing information on dofs needed for the low order methods and FCT.
class DofInfo
{
   Mesh* mesh;
   FiniteElementSpace* fes;

public:

   // For each dof the elements containing that vertex are stored.
   mutable std::map<int, std::vector<int> > map_for_bounds;

   Vector xi_min, xi_max; // min/max values for each dof
   Vector xe_min, xe_max; // min/max values for each element
   
   // TODO should these three be Tables?
   DenseMatrix BdrDofs, Sub2Ind;
   DenseTensor NbrDof;
   
   int dim, numBdrs, numDofs, numSubcells, numDofsSubcell;

   DofInfo(FiniteElementSpace* _fes)
   {
      fes = _fes;
      mesh = fes->GetMesh();
      dim = mesh->Dimension();
      
      int n = fes->GetVSize();
      int ne = mesh->GetNE();
      
      xi_min.SetSize(n);
      xi_max.SetSize(n);
      xe_min.SetSize(ne);
      xe_max.SetSize(ne);

      // Use the first mesh element as indicator.
      const FiniteElement &dummy = *fes->GetFE(0);
      dummy.ExtractBdrDofs(BdrDofs);
      numDofs = BdrDofs.Height();
      numBdrs = BdrDofs.Width();
      
      GetVertexBoundsMap();  // Fill map_for_bounds.
      FillNeighborDofs();    // Fill NbrDof.
      FillSubcell2CellDof(); // Fill Sub2Ind.
   }

   // Computes the admissible interval of values for one dof from the min and
   // max values of all elements that feature a dof at this physical location.
   // It is assumed that a low order method has computed the min/max values for
   // each element.
   void ComputeVertexBounds(const Vector& x, const int dofInd)
   {
      xi_min(dofInd) = numeric_limits<double>::infinity();
      xi_max(dofInd) = -xi_min(dofInd);

      for (int i = 0; i < (int)map_for_bounds[dofInd].size(); i++)
      {
         xi_max(dofInd) = max(xi_max(dofInd),xe_max(map_for_bounds[dofInd][i]));
         xi_min(dofInd) = min(xi_min(dofInd),xe_min(map_for_bounds[dofInd][i]));
      }
   }
   
   // Destructor
   ~DofInfo() { }

private:

   // Returns element sharing a face with both el1 and el2, but is not el.
   // NOTE: This approach will not work for meshes with hanging nodes.
   // NOTE: The same geometry for all elements is assumed.
   int GetCommonElem(int el, int el1, int el2)
   {
      if (min(el1, el2) < 0) { return -1; }

      int i, j, CmnNbr;
      bool found = false;
      Array<int> bdrs1, bdrs2, orientation, NbrEl1, NbrEl2;
      FaceElementTransformations *Trans;

      NbrEl1.SetSize(numBdrs); NbrEl2.SetSize(numBdrs);

      if (dim==1)
      {
         mesh->GetElementVertices(el1, bdrs1);
         mesh->GetElementVertices(el2, bdrs2);
      }
      else if (dim==2)
      {
         mesh->GetElementEdges(el1, bdrs1, orientation);
         mesh->GetElementEdges(el2, bdrs2, orientation);
      }
      else if (dim==3)
      {
         mesh->GetElementFaces(el1, bdrs1, orientation);
         mesh->GetElementFaces(el2, bdrs2, orientation);
      }

      // Get lists of all neighbors of el1 and el2.
      for (i = 0; i < numBdrs; i++)
      {
         Trans = mesh->GetFaceElementTransformations(bdrs1[i]);
         NbrEl1[i] = Trans->Elem1No != el1 ? Trans->Elem1No : Trans->Elem2No;

         Trans = mesh->GetFaceElementTransformations(bdrs2[i]);
         NbrEl2[i] = Trans->Elem1No != el2 ? Trans->Elem1No : Trans->Elem2No;
      }

      for (i = 0; i < numBdrs; i++)
      {
         if (NbrEl1[i] < 0) { continue; }
         for (j = 0; j < numBdrs; j++)
         {
            if (NbrEl2[j] < 0) { continue; }
            
            // add neighbor elements that share a face
            // with el1 and el2 but are not el
            if ((NbrEl1[i] == NbrEl2[j]) && (NbrEl1[i] != el))
            {
               if (!found)
               {
                  CmnNbr = NbrEl1[i];
                  found = true;
               }
               else
               {
                  mfem_error("Found multiple common neighbor elements.");
               }
            }
         }
      }
      if (found)
      {
         return CmnNbr;
      }
      else { return -1; }
   }

   // This fills the map_for_bounds according to our paper.
   // NOTE: The mesh is assumed to consist of segments, quads or hexes.
   // NOTE: This approach will not work for meshes with hanging nodes.
   void GetVertexBoundsMap()
   {
      const FiniteElement &dummy = *fes->GetFE(0);
      int i, j, k, dofInd, nbr;
      int ne = mesh->GetNE(), nd = dummy.GetDof(), p = dummy.GetOrder();
      Array<int> bdrs, orientation, NbrElem;
      FaceElementTransformations *Trans;

      NbrElem.SetSize(numBdrs);

      for (k = 0; k < ne; k++)
      {
         // include the current element for all dofs of the element
         for (i = 0; i < nd; i++)
         {
            dofInd = k*nd+i;
            map_for_bounds[dofInd].push_back(k);
         }

         if (dim==1)
         {
            mesh->GetElementVertices(k, bdrs);
         }
         else if (dim==2)
         {
            mesh->GetElementEdges(k, bdrs, orientation);
         }
         else if (dim==3)
         {
            mesh->GetElementFaces(k, bdrs, orientation);
         }

         // Include neighbors sharing a face with element k for face dofs.
         for (i = 0; i < numBdrs; i++)
         {
            Trans = mesh->GetFaceElementTransformations(bdrs[i]);

            NbrElem[i] = Trans->Elem1No == k ? Trans->Elem2No : Trans->Elem1No;
            
            if (NbrElem[i] < 0) { continue; }
            
            for (j = 0; j < numDofs; j++)
            {
               dofInd = k*nd+BdrDofs(j,i);
               map_for_bounds[dofInd].push_back(NbrElem[i]);
            }
         }

         // Include neighbors that have no face in common with element k.
         if (dim==2) // Include neighbor elements for the four vertices.
         {

            nbr = GetCommonElem(k, NbrElem[3], NbrElem[0]);
            if (nbr >= 0) { map_for_bounds[k*nd].push_back(nbr); }

            nbr = GetCommonElem(k, NbrElem[0], NbrElem[1]);
            if (nbr >= 0) { map_for_bounds[k*nd+p].push_back(nbr); }

            nbr = GetCommonElem(k, NbrElem[1], NbrElem[2]);
            if (nbr >= 0) { map_for_bounds[(k+1)*nd-1].push_back(nbr); }

            nbr = GetCommonElem(k, NbrElem[2], NbrElem[3]);
            if (nbr >= 0) { map_for_bounds[k*nd+p*(p+1)].push_back(nbr); }
         }
         else if (dim==3)
         {
            Array<int> EdgeNbrs; EdgeNbrs.SetSize(12);

            EdgeNbrs[0]  = GetCommonElem(k, NbrElem[0], NbrElem[1]);
            EdgeNbrs[1]  = GetCommonElem(k, NbrElem[0], NbrElem[2]);
            EdgeNbrs[2]  = GetCommonElem(k, NbrElem[0], NbrElem[3]);
            EdgeNbrs[3]  = GetCommonElem(k, NbrElem[0], NbrElem[4]);
            EdgeNbrs[4]  = GetCommonElem(k, NbrElem[5], NbrElem[1]);
            EdgeNbrs[5]  = GetCommonElem(k, NbrElem[5], NbrElem[2]);
            EdgeNbrs[6]  = GetCommonElem(k, NbrElem[5], NbrElem[3]);
            EdgeNbrs[7]  = GetCommonElem(k, NbrElem[5], NbrElem[4]);
            EdgeNbrs[8]  = GetCommonElem(k, NbrElem[4], NbrElem[1]);
            EdgeNbrs[9]  = GetCommonElem(k, NbrElem[1], NbrElem[2]);
            EdgeNbrs[10] = GetCommonElem(k, NbrElem[2], NbrElem[3]);
            EdgeNbrs[11] = GetCommonElem(k, NbrElem[3], NbrElem[4]);

            // include neighbor elements for the twelve edges of a square
            for (j = 0; j <= p; j++)
            {
               if (EdgeNbrs[0] >= 0)
               {
                  map_for_bounds[k*nd+j].push_back(EdgeNbrs[0]);
               }
               if (EdgeNbrs[1] >= 0)
               {
                  map_for_bounds[k*nd+(j+1)*(p+1)-1].push_back(EdgeNbrs[1]);
               }
               if (EdgeNbrs[2] >= 0)
               {
                  map_for_bounds[k*nd+p*(p+1)+j].push_back(EdgeNbrs[2]);
               }
               if (EdgeNbrs[3] >= 0)
               {
                  map_for_bounds[k*nd+j*(p+1)].push_back(EdgeNbrs[3]);
               }
               if (EdgeNbrs[4] >= 0)
               {
                  map_for_bounds[k*nd+(p+1)*(p+1)*p+j].push_back(EdgeNbrs[4]);
               }
               if (EdgeNbrs[5] >= 0)
               {
                  map_for_bounds[k*nd+(p+1)*(p+1)*p+(j+1)*(p+1)-1].push_back(EdgeNbrs[5]);
               }
               if (EdgeNbrs[6] >= 0)
               {
                  map_for_bounds[k*nd+(p+1)*(p+1)*p+p*(p+1)+j].push_back(EdgeNbrs[6]);
               }
               if (EdgeNbrs[7] >= 0)
               {
                  map_for_bounds[k*nd+(p+1)*(p+1)*p+j*(p+1)].push_back(EdgeNbrs[7]);
               }
               if (EdgeNbrs[8] >= 0)
               {
                  map_for_bounds[k*nd+j*(p+1)*(p+1)].push_back(EdgeNbrs[8]);
               }
               if (EdgeNbrs[9] >= 0)
               {
                  map_for_bounds[k*nd+p+j*(p+1)*(p+1)].push_back(EdgeNbrs[9]);
               }
               if (EdgeNbrs[10] >= 0)
               {
                  map_for_bounds[k*nd+(j+1)*(p+1)*(p+1)-1].push_back(EdgeNbrs[10]);
               }
               if (EdgeNbrs[11] >= 0)
               {
                  map_for_bounds[k*nd+p*(p+1)+j*(p+1)*(p+1)].push_back(EdgeNbrs[11]);
               }
            }

            // include neighbor elements for the 8 vertices of a square
            nbr = GetCommonElem(NbrElem[0], EdgeNbrs[0], EdgeNbrs[3]);
            if (nbr >= 0)
            {
               map_for_bounds[k*nd].push_back(nbr);
            }

            nbr = GetCommonElem(NbrElem[0], EdgeNbrs[0], EdgeNbrs[1]);
            if (nbr >= 0)
            {
               map_for_bounds[k*nd+p].push_back(nbr);
            }

            nbr = GetCommonElem(NbrElem[0], EdgeNbrs[2], EdgeNbrs[3]);
            if (nbr >= 0)
            {
               map_for_bounds[k*nd+p*(p+1)].push_back(nbr);
            }

            nbr = GetCommonElem(NbrElem[0], EdgeNbrs[1], EdgeNbrs[2]);
            if (nbr >= 0)
            {
               map_for_bounds[k*nd+(p+1)*(p+1)-1].push_back(nbr);
            }

            nbr = GetCommonElem(NbrElem[5], EdgeNbrs[4], EdgeNbrs[7]);
            if (nbr >= 0)
            {
               map_for_bounds[k*nd+(p+1)*(p+1)*p].push_back(nbr);
            }

            nbr = GetCommonElem(NbrElem[5], EdgeNbrs[4], EdgeNbrs[5]);
            if (nbr >= 0)
            {
               map_for_bounds[k*nd+(p+1)*(p+1)*p+p].push_back(nbr);
            }

            nbr = GetCommonElem(NbrElem[5], EdgeNbrs[6], EdgeNbrs[7]);
            if (nbr >= 0)
            {
               map_for_bounds[k*nd+(p+1)*(p+1)*p+(p+1)*p].push_back(nbr);
            }

            nbr = GetCommonElem(NbrElem[5], EdgeNbrs[5], EdgeNbrs[6]);
            if (nbr >= 0)
            {
               map_for_bounds[k*nd+(p+1)*(p+1)*(p+1)-1].push_back(nbr);
            }
         }
      }
   }
   
   // For each DOF on an element boundary, the global index of the DOF on the
   // opposite site is computed and stored in a list. This is needed for 
   // lumping the flux contributions as in the paper. Right now it works on
   // 1D meshes, quad meshes in 2D and 3D meshes of ordered cubes.
   // NOTE: The mesh is assumed to consist of segments, quads or hexes.
   // NOTE: This approach will not work for meshes with hanging nodes.
   void FillNeighborDofs()
   {
      // Use the first mesh element as indicator.
      const FiniteElement &dummy = *fes->GetFE(0);
      int i, j, k, ind, nbr, ne = mesh->GetNE();
      int nd = dummy.GetDof(), p = dummy.GetOrder();
      Array <int> bdrs, NbrBdrs, orientation;
      FaceElementTransformations *Trans;
      
      NbrDof.SetSize(ne, numBdrs, numDofs);
      
      for (k = 0; k < ne; k++)
      {
         if (dim==1)
         {
            mesh->GetElementVertices(k, bdrs);
            
            for (i = 0; i < numBdrs; i++)
            {
               Trans = mesh->GetFaceElementTransformations(bdrs[i]);
               nbr = Trans->Elem1No == k ? Trans->Elem2No : Trans->Elem1No;
               NbrDof(k,i,0) = nbr*nd + BdrDofs(0,(i+1)%2);
            }
         }
         else if (dim==2)
         {
            mesh->GetElementEdges(k, bdrs, orientation);
            
            for (i = 0; i < numBdrs; i++)
            {
               Trans = mesh->GetFaceElementTransformations(bdrs[i]);
               nbr = Trans->Elem1No == k ? Trans->Elem2No : Trans->Elem1No;
               
               for (j = 0; j < numDofs; j++)
               {
                  if (nbr >= 0)
                  {
                     mesh->GetElementEdges(nbr, NbrBdrs, orientation);
                     // Find the local index ind in nbr of the common face.
                     for (ind = 0; ind < numBdrs; ind++)
                     {
                        if (NbrBdrs[ind] == bdrs[i]) { break; }
                     }
                     // Here it is utilized that the orientations of the face
                     // for the two elements are opposite of each other.
                     NbrDof(k,i,j) = nbr*nd + BdrDofs(numDofs-1-j,ind);
                  }
                  else
                  {
                     NbrDof(k,i,j) = -1;
                  }
               }
            }
         }
         else if (dim==3)
         {
            mesh->GetElementFaces(k, bdrs, orientation);
            
            // TODO: This works only for meshes of cubes with uniformly ordered
            // nodes.
            for (j = 0; j < numDofs; j++)
            {
               Trans = mesh->GetFaceElementTransformations(bdrs[0]);
               nbr = Trans->Elem1No == k ? Trans->Elem2No : Trans->Elem1No;

               NbrDof(k,0,j) = nbr*nd + (p+1)*(p+1)*p+j;

               Trans = mesh->GetFaceElementTransformations(bdrs[1]);
               nbr = Trans->Elem1No == k ? Trans->Elem2No : Trans->Elem1No;

               NbrDof(k,1,j) = nbr*nd + (j/(p+1))*(p+1)*(p+1)+(p+1)*p+(j%(p+1));

               Trans = mesh->GetFaceElementTransformations(bdrs[2]);
               nbr = Trans->Elem1No == k ? Trans->Elem2No : Trans->Elem1No;

               NbrDof(k,2,j) = nbr*nd + j*(p+1);

               Trans = mesh->GetFaceElementTransformations(bdrs[3]);
               nbr = Trans->Elem1No == k ? Trans->Elem2No : Trans->Elem1No;

               NbrDof(k,3,j) = nbr*nd + (j/(p+1))*(p+1)*(p+1)+(j%(p+1));

               Trans = mesh->GetFaceElementTransformations(bdrs[4]);
               nbr = Trans->Elem1No == k ? Trans->Elem2No : Trans->Elem1No;

               NbrDof(k,4,j) = nbr*nd + (j+1)*(p+1)-1;

               Trans = mesh->GetFaceElementTransformations(bdrs[5]);
               nbr = Trans->Elem1No == k ? Trans->Elem2No : Trans->Elem1No;

               NbrDof(k,5,j) = nbr*nd + j;
            }
         }
      }
   }
   
   // A list is filled to later access the correct element-global
   // indices given the subcell number and subcell index.
   // NOTE: The mesh is assumed to consist of segments, quads or hexes.
   void FillSubcell2CellDof()
   {
      const FiniteElement &dummy = *fes->GetFE(0);
      int j, m, aux, p = dummy.GetOrder();
      
      if (dim==1)
      {
         numSubcells = p;
         numDofsSubcell = 2;
      }
      else if (dim==2)
      {
         numSubcells = p*p;
         numDofsSubcell = 4;
      }
      else if (dim==3)
      {
         numSubcells = p*p*p;
         numDofsSubcell = 8;
      }
      
      Sub2Ind.SetSize(numSubcells, numDofsSubcell);
      
      for (m = 0; m < numSubcells; m++)
      {
         for (j = 0; j < numDofsSubcell; j++)
         {
            if (dim == 1)
            {
               Sub2Ind(m,j) = m + j;
            }
            else if (dim == 2)
            {
               aux = m + (m/p);
               switch (j)
               {
                  case 0: Sub2Ind(m,j) =  aux; break;
                  case 1: Sub2Ind(m,j) =  aux + 1; break;
                  case 2: Sub2Ind(m,j) =  aux + p+1; break;
                  case 3: Sub2Ind(m,j) =  aux + p+2; break;
               }
            }
            else if (dim == 3)
            {
               aux = m + (m/p)+(p+1)*(m/(p*p));
               switch (j)
               {
                  case 0:
                     Sub2Ind(m,j) = aux; break;
                  case 1:
                     Sub2Ind(m,j) = aux + 1; break;
                  case 2:
                     Sub2Ind(m,j) = aux + p+1; break;
                  case 3:
                     Sub2Ind(m,j) = aux + p+2; break;
                  case 4:
                     Sub2Ind(m,j) = aux + (p+1)*(p+1); break;
                  case 5:
                     Sub2Ind(m,j) = aux + (p+1)*(p+1)+1; break;
                  case 6:
                     Sub2Ind(m,j) = aux + (p+1)*(p+1)+p+1; break;
                  case 7:
                     Sub2Ind(m,j) = aux + (p+1)*(p+1)+p+2; break;
               }
            }
         }
      }
   }
};


class Assembly
{
public:
   Assembly(DofInfo &_dofs, LowOrderMethod &lom) :
            fes(lom.fes), dofs(_dofs)
   {
      Mesh *mesh = fes->GetMesh();
      int k, i, m, nd, dim = mesh->Dimension(), ne = fes->GetNE();
      
      // Use the first mesh element as indicator.
      const FiniteElement &dummy = *fes->GetFE(0);
      nd = dummy.GetDof();
      
      Array <int> bdrs, orientation;
      FaceElementTransformations *Trans;

      const bool NeedBdr = lom.OptScheme || ( (lom.MonoType != DiscUpw)
                                       && (lom.MonoType != DiscUpw_FCT) );

      const bool NeedSubcells = lom.OptScheme && (( lom.MonoType == ResDist)
                                            || (lom.MonoType == ResDist_FCT) );
      
      if (NeedBdr)
      {
         bdrInt.SetSize(ne, dofs.numBdrs, dofs.numDofs*dofs.numDofs);
         bdrInt = 0.;
      }
      if (NeedSubcells)
      {
         VolumeTerms = lom.VolumeTerms;
         SubcellWeights.SetSize(ne, dofs.numSubcells, dofs.numDofsSubcell);
         
         if (exec_mode == 0)
         {
            SubFes0 = lom.SubFes0;
            SubFes1 = lom.SubFes1;
            subcell_mesh = lom.subcell_mesh;
         }
      }
      
      // Initialization for transport mode.
      if ((exec_mode == 0) && (NeedBdr || NeedSubcells))
      {
         for (k = 0; k < ne; k++)
         {
            if (NeedBdr)
            {
               if (dim==1)
               {
                  mesh->GetElementVertices(k, bdrs);
               }
               else if (dim==2)
               {
                  mesh->GetElementEdges(k, bdrs, orientation);
               }
               else if (dim==3)
               {
                  mesh->GetElementFaces(k, bdrs, orientation);
               }
               
               for (i = 0; i < dofs.numBdrs; i++)
               {
                  Trans = mesh->GetFaceElementTransformations(bdrs[i]);
                  ComputeFluxTerms(k, i, Trans, lom);
               }
            }
            if (NeedSubcells)
            {
               for (m = 0; m < dofs.numSubcells; m++)
               {
                  ComputeSubcellWeights(k, m);
               }
            }
         }
      }
   }

   // Destructor
   ~Assembly() { }

   // Auxiliary member variables that need to be accessed during time-stepping.
   FiniteElementSpace *fes, *SubFes0, *SubFes1;
   DofInfo &dofs;
   Mesh *subcell_mesh;
   BilinearFormIntegrator *VolumeTerms;
   
   // Data structures storing Galerkin contributions. These are updated for 
   // remap but remain constant for transport.
   DenseTensor bdrInt, SubcellWeights;
   
   void ComputeFluxTerms(const int k, const int BdrID,
                         FaceElementTransformations *Trans, LowOrderMethod &lom)
   {
      Mesh *mesh = fes->GetMesh();
      
      int i, j, l, nd, dim = mesh->Dimension();
      double aux, vn;
      
      const FiniteElement &el = *fes->GetFE(k);
      nd = el.GetDof();
      
      Vector vval, nor(dim), shape(nd);
      
      for (l = 0; l < lom.irF->GetNPoints(); l++)
      {
         const IntegrationPoint &ip = lom.irF->IntPoint(l);
         IntegrationPoint eip1;
         Trans->Face->SetIntPoint(&ip);
         
         if (dim == 1)
         {
            Trans->Loc1.Transform(ip, eip1);
            nor(0) = 2.*eip1.x - 1.0;
         }
         else
         {
            CalcOrtho(Trans->Face->Jacobian(), nor);
         }
         
         if (Trans->Elem1No != k)
         {
            Trans->Loc2.Transform(ip, eip1);
            el.CalcShape(eip1, shape);
            Trans->Elem2->SetIntPoint(&eip1);
            lom.coef->Eval(vval, *Trans->Elem2, eip1);
            nor *= -1.;
            Trans->Loc1.Transform(ip, eip1);
         }
         else
         {
            Trans->Loc1.Transform(ip, eip1);
            el.CalcShape(eip1, shape);
            Trans->Elem1->SetIntPoint(&eip1);
            lom.coef->Eval(vval, *Trans->Elem1, eip1);
            Trans->Loc2.Transform(ip, eip1);
         }
         
         nor /= nor.Norml2();
         
         if (exec_mode == 0)
         {
            // Transport.
            vn = min(0., vval * nor);
         }
         else
         {
            // Remap.
            vn = max(0., vval * nor);
            vn *= -1.0;
         }
         
         for (i = 0; i < dofs.numDofs; i++)
         {
            aux = ip.weight * Trans->Face->Weight()
                  * shape(dofs.BdrDofs(i,BdrID)) * vn;

            for (j = 0; j < dofs.numDofs; j++)
            {
               bdrInt(k, BdrID, i*dofs.numDofs+j) -= aux
                                    * shape(dofs.BdrDofs(j,BdrID));
            }
         }
      }
   }
   
   void ComputeSubcellWeights(const int k, const int m)
   {
      DenseMatrix elmat; // These are essentially the same.
      int dofInd = k*dofs.numSubcells+m;
      const FiniteElement *el0 = SubFes0->GetFE(dofInd);
      const FiniteElement *el1 = SubFes1->GetFE(dofInd);
      ElementTransformation *tr = subcell_mesh->GetElementTransformation(dofInd);
      VolumeTerms->AssembleElementMatrix2(*el1, *el0, *tr, elmat);
      
      for (int j = 0; j < elmat.Width(); j++)
      {
         // Using the fact that elmat has just one row.
         SubcellWeights(k,m,j) = elmat(0,j);
      }
   }
};


/** A time-dependent operator for the right-hand side of the ODE. The DG weak
    form of du/dt = -v.grad(u) is M du/dt = K u + b, where M and K are the mass
    and advection matrices, and b describes the flow on the boundary. This can
    be written as a general ODE, du/dt = M^{-1} (K u + b), and this class is
    used to evaluate the right-hand side. */
class FE_Evolution : public TimeDependentOperator
{
private:
   BilinearForm &Mbf, &Kbf, &ml;
   SparseMatrix &M, &K;
   Vector &lumpedM;
   const Vector &b;

   Vector start_pos;
   GridFunction &mesh_pos, &vel_pos;

   mutable Vector z;

   double dt;
   Assembly &asmbl;
   
   LowOrderMethod &lom;
   DofInfo &dofs;

public:
   FE_Evolution(BilinearForm &Mbf_, SparseMatrix &_M, BilinearForm &_ml,
                Vector &_lumpedM, BilinearForm &Kbf_, SparseMatrix &_K,
                const Vector &_b, GridFunction &mpos, GridFunction &vpos,
                Assembly &_asmbl, LowOrderMethod &_lom, DofInfo &_dofs);

   virtual void Mult(const Vector &x, Vector &y) const;

   virtual void SetDt(double _dt) { dt = _dt; }
   void SetRemapStartPos(const Vector &spos) { start_pos = spos; }
   void GetRemapStartPos(Vector &spos) { spos = start_pos; }

   // Mass matrix solve, addressing the bad Bernstein condition number.
   virtual void NeumannSolve(const Vector &b, Vector &x) const;

   virtual void LinearFluxLumping(const int k, const int nd,
                                  const int BdrID, const Vector &x,
                                  Vector &y, const Vector &alpha) const;

   virtual void ComputeHighOrderSolution(const Vector &x, Vector &y) const;
   virtual void ComputeLowOrderSolution(const Vector &x, Vector &y) const;
   virtual void ComputeFCTSolution(const Vector &x, const Vector &yH,
                                   const Vector &yL, Vector &y) const;

   virtual ~FE_Evolution() { }
};

FE_Evolution* adv;

int main(int argc, char *argv[])
{
   // 1. Parse command-line options.
  
#ifdef USE_LUA
   L = luaL_newstate();
   luaL_openlibs(L);
   const char* problem_file = "problem.lua";
#else
   problem_num = 4;
#endif
   const char *mesh_file = "./data/unit-square.mesh";
   int ref_levels = 2;
   int order = 3;
   int ode_solver_type = 3;
   MONOTYPE MonoType = ResDist_FCT;
   bool OptScheme = true;
   double t_final = 2.0;
   double dt = 0.0025;
   bool visualization = true;
   bool visit = false;
   bool binary = false;
   int vis_steps = 100;

   int precision = 8;
   cout.precision(precision);

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
#ifdef USE_LUA
   args.AddOption(&problem_file, "-p", "--problem",
                  "lua problem definition file.");
#else
   args.AddOption(&problem_num, "-p", "--problem",
                  "Problem setup to use. See options in velocity_function().");
#endif
   args.AddOption(&ref_levels, "-r", "--refine",
                  "Number of times to refine the mesh uniformly.");
   args.AddOption(&order, "-o", "--order",
                  "Order (degree) of the finite elements.");
   args.AddOption(&ode_solver_type, "-s", "--ode-solver",
                  "ODE solver: 1 - Forward Euler,\n\t"
                  "            2 - RK2 SSP, 3 - RK3 SSP, 4 - RK4, 6 - RK6.");
   args.AddOption((int*)(&MonoType), "-mt", "--MonoType",
                  "Monotonicity scheme: 0 - no monotonicity treatment,\n\t"
                  "                     1 - discrete upwinding - LO,\n\t"
                  "                     2 - discrete upwinding - FCT,\n\t"
                  "                     3 - residual distribution - LO,\n\t"
                  "                     4 - residual distribution - FCT.");
   args.AddOption(&OptScheme, "-sc", "--subcell", "-el", "--element (basic)",
                  "Optimized scheme: PDU / subcell (optimized).");
   args.AddOption(&t_final, "-tf", "--t-final",
                  "Final time; start time is 0.");
   args.AddOption(&dt, "-dt", "--time-step",
                  "Time step.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&visit, "-visit", "--visit-datafiles", "-no-visit",
                  "--no-visit-datafiles",
                  "Save data files for VisIt (visit.llnl.gov) visualization.");
   args.AddOption(&binary, "-binary", "--binary-datafiles", "-ascii",
                  "--ascii-datafiles",
                  "Use binary (Sidre) or ascii format for VisIt data files.");
   args.AddOption(&vis_steps, "-vs", "--visualization-steps",
                  "Visualize every n-th timestep.");
   args.Parse();
   if (!args.Good())
   {
      args.PrintUsage(cout);
      return 1;
   }
   args.PrintOptions(cout);

   // When not using lua, exec mode is derived from problem number convention
   if (problem_num < 10)      { exec_mode = 0; }
   else if (problem_num < 20) { exec_mode = 1; }
   else { MFEM_ABORT("Unspecified execution mode."); }

#ifdef USE_LUA
   // When using lua, exec mode is read from lua file
   if (luaL_dofile(L, problem_file)) {
      printf("Error opening lua file: %s\n",problem_file);
      exit(1);
   }

   lua_getglobal(L, "exec_mode");
   if (!lua_isnumber(L, -1)) {
      printf("Did not find exec_mode in lua input.\n");
      return 1;
   }
   exec_mode = (int)lua_tonumber(L, -1);
#endif

   // 2. Read the mesh from the given mesh file. We can handle geometrically
   //    periodic meshes in this code.
   Mesh *mesh = new Mesh(mesh_file, 1, 1);

   const int dim = mesh->Dimension();

   
   // 3. Define the ODE solver used for time integration. Several explicit
   //    Runge-Kutta methods are available.
   ODESolver *ode_solver = NULL;
   switch (ode_solver_type)
   {
      case 1: ode_solver = new ForwardEulerSolver; break;
      case 2: ode_solver = new RK2Solver(1.0); break;
      case 3: ode_solver = new RK3SSPSolver; break;
      case 4: ode_solver = new RK4Solver; break;
      case 6: ode_solver = new RK6Solver; break;
      default:
         cout << "Unknown ODE solver type: " << ode_solver_type << '\n';
         delete mesh;
         return 3;
   }

   // 4. Refine the mesh to increase the resolution. In this example we do
   //    'ref_levels' of uniform refinement, where 'ref_levels' is a
   //    command-line parameter. If the mesh is of NURBS type, we convert it to
   //    a (piecewise-polynomial) high-order mesh.
   for (int lev = 0; lev < ref_levels; lev++)
   {
      mesh->UniformRefinement();
   }
   if (mesh->NURBSext)
   {
      mesh->SetCurvature(max(order, 1));
   }
   mesh->GetBoundingBox(bb_min, bb_max, max(order, 1));

   // Current mesh positions.
   GridFunction *x = mesh->GetNodes();

   // Store initial positions.
   Vector x0(x->Size());
   x0 = *x;

   // 5. Define the discontinuous DG finite element space of the given
   //    polynomial order on the refined mesh.
   const int btype = BasisType::Positive;
   DG_FECollection fec(order, dim, btype);
   FiniteElementSpace fes(mesh, &fec);

   // Check for meaningful combinations of parameters.
   bool fail = false;
   if (MonoType != None)
   {
      if (((int)MonoType != MonoType) || (MonoType < 0) || (MonoType > 4))
      {
         cout << "Unsupported option for monotonicity treatment." << endl;
         fail = true;
      }
      if (btype != 2)
      {
         cout << "Monotonicity treatment requires Bernstein basis." << endl;
         fail = true;
      }
      if (order == 0)
      {
         // Disable monotonicity treatment for piecwise constants.
         mfem_warning("For -o 0, monotonicity treatment is disabled.");
         MonoType = None;
         OptScheme = false;
      }
   }
   else { OptScheme = false; }
   
   if ((MonoType > 2) && (order==1) && OptScheme)
   {
      // Avoid subcell methods for linear elements.
      mfem_warning("For -o 1, subcell scheme is disabled.");
      OptScheme = false;
   }
   if (fail)
   {
      delete mesh;
      delete ode_solver;
      return 5;
   }

   cout << "Number of unknowns: " << fes.GetVSize() << endl;

   // 6. Set up and assemble the bilinear and linear forms corresponding to the
   //    DG discretization. The DGTraceIntegrator involves integrals over mesh
   //    interior faces.
   //    Also prepare for the use of low and high order schemes.
   VectorFunctionCoefficient velocity(dim, velocity_function);
   FunctionCoefficient inflow(inflow_function);
   FunctionCoefficient u0(u0_function);

   // Mesh velocity.
   GridFunction v_gf(x->FESpace());
   v_gf.ProjectCoefficient(velocity);
   if (mesh->bdr_attributes.Size() > 0)
   {
      // Zero it out on boundaries (not moving boundaries).
      Array<int> ess_bdr(mesh->bdr_attributes.Max()), ess_vdofs;
      ess_bdr = 1;
      x->FESpace()->GetEssentialVDofs(ess_bdr, ess_vdofs);
      for (int i = 0; i < v_gf.Size(); i++)
      {
         if (ess_vdofs[i] == -1) { v_gf(i) = 0.0; }
      }
   }
   VectorGridFunctionCoefficient v_coef(&v_gf);

   BilinearForm m(&fes);
   m.AddDomainIntegrator(new MassIntegrator);

   BilinearForm k(&fes);

   if (exec_mode == 0)
   {
      k.AddDomainIntegrator(new ConvectionIntegrator(velocity, -1.0));
   }
   else if (exec_mode == 1)
   {
      k.AddDomainIntegrator(new ConvectionIntegrator(v_coef));
   }
   
   // In case of basic discrete upwinding, add boundary terms.
   if (((MonoType == DiscUpw) || (MonoType == DiscUpw_FCT)) && (!OptScheme))
   {
      if (exec_mode == 0)
      {
         k.AddInteriorFaceIntegrator( new TransposeIntegrator(
            new DGTraceIntegrator(velocity, 1.0, -0.5)) );
         k.AddBdrFaceIntegrator( new TransposeIntegrator(
            new DGTraceIntegrator(velocity, 1.0, -0.5)) );
      }
      else if (exec_mode == 1)
      {
         k.AddInteriorFaceIntegrator(new TransposeIntegrator(
            new DGTraceIntegrator(v_coef, -1.0, -0.5)) );
         k.AddBdrFaceIntegrator( new TransposeIntegrator(
            new DGTraceIntegrator(v_coef, -1.0, -0.5)) );
      }
   }

   // Compute the lumped mass matrix algebraicly
   Vector lumpedM;
   BilinearForm ml(&fes);
   ml.AddDomainIntegrator(new LumpedIntegrator(new MassIntegrator));
   ml.Assemble();
   ml.Finalize();
   ml.SpMat().GetDiag(lumpedM);

   LinearForm b(&fes);
   b.AddBdrFaceIntegrator(
      new BoundaryFlowIntegrator(inflow, v_coef, -1.0, -0.5));

   m.Assemble();
   m.Finalize();
   int skip_zeros = 0;
   k.Assemble(skip_zeros);
   k.Finalize(skip_zeros);
   b.Assemble();

   // Store topological dof data.
   DofInfo dofs(&fes);

   // Precompute data required for high and low order schemes. This could be put
   // into a seperate routine. I am using a struct now because the various
   // schemes require quite different information.
   LowOrderMethod lom;
   lom.MonoType = MonoType;
   lom.OptScheme = OptScheme;
   lom.fes = &fes;

   if ((lom.MonoType == DiscUpw) || (lom.MonoType == DiscUpw_FCT))
   {
      if (!lom.OptScheme)
      {
         lom.smap = SparseMatrix_Build_smap(k.SpMat());
         lom.D = k.SpMat();
         
         if (exec_mode == 0)
         {
            ComputeDiscreteUpwindingMatrix(k.SpMat(), lom.smap, lom.D);
         }
      }
      else
      {
         lom.pk = new BilinearForm(&fes);
         if (exec_mode == 0)
         {
            lom.pk->AddDomainIntegrator(
               new PrecondConvectionIntegrator(velocity, -1.0) );
         }
         else if (exec_mode == 1)
         {
            lom.pk->AddDomainIntegrator(
               new PrecondConvectionIntegrator(v_coef) );
         }
         lom.pk->Assemble(skip_zeros);
         lom.pk->Finalize(skip_zeros);
         
         lom.smap = SparseMatrix_Build_smap(lom.pk->SpMat());
         lom.D = lom.pk->SpMat();
         
         if (exec_mode == 0)
         {
            ComputeDiscreteUpwindingMatrix(lom.pk->SpMat(), lom.smap, lom.D);
         }
      }
   }
   if (exec_mode == 1) { lom.coef = &v_coef; }
   else { lom.coef = &velocity; }
   
   lom.irF = GetFaceIntRule(&fes);
   
   DG_FECollection fec0(0, dim, btype);
   DG_FECollection fec1(1, dim, btype);
   
   // For linear elements, Opt scheme has already been disabled.
   const bool NeedSubcells = lom.OptScheme && (lom.MonoType == ResDist ||
                                               lom.MonoType == ResDist_FCT);

   if (NeedSubcells)
   {
      if (exec_mode == 0)
      {
         lom.VolumeTerms = new MixedConvectionIntegrator(velocity, -1.0);
      }
      else if (exec_mode == 1)
      {
         // TODO Figure out why this gives a seg-fault, it should be v_coef, as
         //      it is v_coef for the high order bilinearform k.
         // lom.VolumeTerms = new MixedConvectionIntegrator(v_coef);
         lom.VolumeTerms = new MixedConvectionIntegrator(velocity);
      }
      
      lom.fec0 = &fec0;
      lom.fec1 = &fec1;

      if (exec_mode == 0)
      {
         lom.subcell_mesh = GetSubcellMesh(mesh, order);
         lom.SubFes0 = new FiniteElementSpace(lom.subcell_mesh, lom.fec0);
         lom.SubFes1 = new FiniteElementSpace(lom.subcell_mesh, lom.fec1);
      }
   }

   Assembly asmbl(dofs, lom);

   // 7. Define the initial conditions, save the corresponding grid function to
   //    a file and (optionally) save data in the VisIt format and initialize
   //    GLVis visualization.
   GridFunction u(&fes);
   u.ProjectCoefficient(u0);

   {
      ofstream omesh("remhos.mesh");
      omesh.precision(precision);
      mesh->Print(omesh);
      ofstream osol("remhos-init.gf");
      osol.precision(precision);
      u.Save(osol);
   }

   // Create data collection for solution output: either VisItDataCollection for
   // ascii data files, or SidreDataCollection for binary data files.
   DataCollection *dc = NULL;
   if (visit)
   {
      if (binary)
      {
#ifdef MFEM_USE_SIDRE
         dc = new SidreDataCollection("Example9", mesh);
#else
         MFEM_ABORT("Must build with MFEM_USE_SIDRE=YES for binary output.");
#endif
      }
      else
      {
         dc = new VisItDataCollection("Example9", mesh);
         dc->SetPrecision(precision);
      }
      dc->RegisterField("solution", &u);
      dc->SetCycle(0);
      dc->SetTime(0.0);
      dc->Save();
   }

   socketstream sout;
   if (visualization)
   {
      char vishost[] = "localhost";
      int  visport   = 19916;
      sout.open(vishost, visport);
      if (!sout)
      {
         cout << "Unable to connect to GLVis server at "
              << vishost << ':' << visport << endl;
         visualization = false;
         cout << "GLVis visualization disabled.\n";
      }
      else
      {
         sout.precision(precision);
         sout << "solution\n" << *mesh << u;
         sout << "pause\n";
         sout << flush;
         cout << "GLVis visualization paused."
              << " Press space (in the GLVis window) to resume it.\n";
      }
   }

   // check for conservation
   Vector mass(lumpedM);
   double initialMass = lumpedM * u;

   // 8. Define the time-dependent evolution operator describing the ODE
   //    right-hand side, and perform time-integration (looping over the time
   //    iterations, ti, with a time-step dt).

   FE_Evolution* adv = new FE_Evolution(m, m.SpMat(), ml, lumpedM, k,
					k.SpMat(), b, *x, v_gf, asmbl,
					lom, dofs);

   double t = 0.0;
   adv->SetTime(t);
   ode_solver->Init(*adv);

   bool done = false;
   for (int ti = 0; !done; )
   {
      double dt_real = min(dt, t_final - t);

      adv->SetDt(dt_real);

      if (exec_mode == 1) { adv->SetRemapStartPos(x0); }

      ode_solver->Step(u, t, dt_real);
      ti++;

      if (exec_mode == 1) { add(x0, t, v_gf, *x); }

      done = (t >= t_final - 1.e-8*dt);

      if (done || ti % vis_steps == 0)
      {
         cout << "time step: " << ti << ", time: " << t << endl;

         if (visualization) { sout << "solution\n" << *mesh << u << flush; }

         if (visit)
         {
            dc->SetCycle(ti);
            dc->SetTime(t);
            dc->Save();
         }
      }
   }

   // 9. Save the final solution. This output can be viewed later using GLVis:
   //    "glvis -m remhos.mesh -g remhos-final.gf".
   {
      ofstream osol("remhos-final.gf");
      osol.precision(precision);
      u.Save(osol);
   }

   // check for conservation
   double finalMass;
   if (exec_mode == 1)
   {
      ml.BilinearForm::operator=(0.0);
      ml.Assemble();
      ml.SpMat().GetDiag(lumpedM);
      finalMass = lumpedM * u;
   }
   else { finalMass = mass * u; }
   cout << setprecision(10)
        << "Final mass: " << finalMass << endl
        << "Max value:  " << u.Max() << endl << setprecision(6)
        << "Mass loss:  " << abs(initialMass - finalMass) << endl;

   // Compute errors, if the initial condition is equal to the final solution
   if (problem_num == 4) // solid body rotation
   {
      cout << "L1-error: " << u.ComputeLpError(1., u0) << ", L-Inf-error: "
           << u.ComputeLpError(numeric_limits<double>::infinity(), u0)
           << "." << endl;
   }

   // 10. Free the used memory.
   delete mesh;
   delete ode_solver;
   delete dc;
   
   if ( lom.OptScheme && ( (lom.MonoType == DiscUpw)
                        || (lom.MonoType = DiscUpw_FCT) ) )
   {
      delete lom.pk;
   }
   
   if (NeedSubcells)
   {
      delete asmbl.SubFes0;
      delete asmbl.SubFes1;
      delete asmbl.VolumeTerms;
      delete asmbl.subcell_mesh;
   }

   return 0;
}


void FE_Evolution::NeumannSolve(const Vector &f, Vector &x) const
{
   int i, iter, n = f.Size(), max_iter = 20;
   Vector y;
   double resid = f.Norml2(), abs_tol = 1.e-4;

   y.SetSize(n);
   x = 0.;

   for (iter = 1; iter <= max_iter; iter++)
   {
      M.Mult(x, y);
      y -= f;
      resid = y.Norml2();
      if (resid <= abs_tol)
      {
         return;
      }
      for (i = 0; i < n; i++)
      {
         x(i) -= y(i) / lumpedM(i);
      }
   }
}

void FE_Evolution::LinearFluxLumping(const int k, const int nd,
                                     const int BdrID, const Vector &x,
                                     Vector &y, const Vector &alpha) const
{
   int i, j, idx, dofInd;
   double xNeighbor;
   Vector xDiff(dofs.numDofs);

   for (j = 0; j < dofs.numDofs; j++)
   {
      dofInd = k*nd+dofs.BdrDofs(j,BdrID);
      idx = dofs.NbrDof(k, BdrID, j);
      // If NbrDof is -1 and bdrInt > 0., this is an inflow boundary. If NbrDof
      // is -1 and bdrInt = 0., this is an outflow, which is handled correctly.
      // TODO use inflow instead of xNeighbor = 0.
      xNeighbor = idx < 0 ? 0. : x(idx);
      xDiff(j) = xNeighbor - x(dofInd);
   }

   for (i = 0; i < dofs.numDofs; i++)
   {
      dofInd = k*nd+dofs.BdrDofs(i,BdrID);
      for (j = 0; j < dofs.numDofs; j++)
      {
         // alpha=0 is the low order solution, alpha=1, the Galerkin solution.
         // 0 < alpha < 1 can be used for limiting within the low order method.
         y(dofInd) += asmbl.bdrInt(k, BdrID, i*dofs.numDofs + j)
          * ( xDiff(i) + (xDiff(j)-xDiff(i)) * alpha(dofs.BdrDofs(i,BdrID))
                                             * alpha(dofs.BdrDofs(j,BdrID)) );
      }
   }
}

void FE_Evolution::ComputeLowOrderSolution(const Vector &x, Vector &y) const
{
   const FiniteElement* dummy = lom.fes->GetFE(0);
   int i, j, k, dofInd, nd = dummy->GetDof(), ne = lom.fes->GetNE();
   Vector alpha(nd); alpha = 0.;
   
   if ( (lom.MonoType == DiscUpw) || (lom.MonoType == DiscUpw_FCT) )
   {
      // Reassemble on the new mesh (given by mesh_pos).
      if (exec_mode == 1)
      {
         if (!lom.OptScheme)
         {
            ComputeDiscreteUpwindingMatrix(K, lom.smap, lom.D);
         }
         else
         {
            lom.pk->BilinearForm::operator=(0.0);
            lom.pk->Assemble();
            ComputeDiscreteUpwindingMatrix(lom.pk->SpMat(), lom.smap, lom.D);
         }
      }

      // Discretization and monotonicity terms.
      lom.D.Mult(x, y);
      y += b;

      // Lump fluxes (for PDU), compute min/max, and invert lumped mass matrix.
      for (k = 0; k < ne; k++)
      {
         ////////////////////////////
         // Boundary contributions //
         //////////////////////////// 
         if (lom.OptScheme)
         {
            for (i = 0; i < dofs.numBdrs; i++)
            {
               LinearFluxLumping(k, nd, i, x, y, alpha);
            }
         }
         
         dofs.xe_min(k) = numeric_limits<double>::infinity();
         dofs.xe_max(k) = -dofs.xe_min(k);
         
         for (j = 0; j < nd; j++)
         {
            dofInd = k*nd+j;
            dofs.xe_max(k) = max(dofs.xe_max(k), x(dofInd));
            dofs.xe_min(k) = min(dofs.xe_min(k), x(dofInd));
            y(dofInd) /= lumpedM(dofInd);
         }
      }
   }
   else // RD(S)
   {
      Mesh *mesh = lom.fes->GetMesh();
      int m, dofInd2, loc, dim(mesh->Dimension());
      double xSum, xNeighbor, sumFluctSubcellP, sumFluctSubcellN, sumWeightsP,
             sumWeightsN, weightP, weightN, rhoP, rhoN, gammaP, gammaN,
             minGammaP, minGammaN, aux, fluct, gamma = 10., eps = 1.E-15;
      Vector xMaxSubcell, xMinSubcell, sumWeightsSubcellP, sumWeightsSubcellN,
             fluctSubcellP, fluctSubcellN, nodalWeightsP, nodalWeightsN;
            
      // Discretization terms
      y = b;
      K.Mult(x, z);
      
      if ((exec_mode == 1) && (lom.OptScheme))
      {
         // TODO efficiency.
         delete asmbl.subcell_mesh;
         delete asmbl.SubFes0;
         delete asmbl.SubFes1;
         
         asmbl.subcell_mesh = GetSubcellMesh(mesh, dummy->GetOrder());
         asmbl.SubFes0 =  new FiniteElementSpace(asmbl.subcell_mesh, lom.fec0);
         asmbl.SubFes1 =  new FiniteElementSpace(asmbl.subcell_mesh, lom.fec1);
      }

      // Monotonicity terms
      for (k = 0; k < ne; k++)
      {
         ////////////////////////////
         // Boundary contributions //
         ////////////////////////////           
         for (i = 0; i < dofs.numBdrs; i++)
         {
            LinearFluxLumping(k, nd, i, x, y, alpha);
         }
         
         ///////////////////////////
         // Element contributions //
         ///////////////////////////
         dofs.xe_min(k) = numeric_limits<double>::infinity();
         dofs.xe_max(k) = -dofs.xe_min(k);
         rhoP = rhoN = xSum = 0.;

         for (j = 0; j < nd; j++)
         {
            dofInd = k*nd+j;
            dofs.xe_max(k) = max(dofs.xe_max(k), x(dofInd));
            dofs.xe_min(k) = min(dofs.xe_min(k), x(dofInd));
            xSum += x(dofInd);
            if (lom.OptScheme)
            {
               rhoP += max(0., z(dofInd));
               rhoN += min(0., z(dofInd));
            }
         }

         sumWeightsP = nd*dofs.xe_max(k) - xSum + eps;
         sumWeightsN = nd*dofs.xe_min(k) - xSum - eps;

         if (lom.OptScheme)
         {
            fluctSubcellP.SetSize(dofs.numSubcells);
            fluctSubcellN.SetSize(dofs.numSubcells);
            xMaxSubcell.SetSize(dofs.numSubcells);
            xMinSubcell.SetSize(dofs.numSubcells);
            sumWeightsSubcellP.SetSize(dofs.numSubcells);
            sumWeightsSubcellN.SetSize(dofs.numSubcells);
            nodalWeightsP.SetSize(nd);
            nodalWeightsN.SetSize(nd);
            sumFluctSubcellP = sumFluctSubcellN = 0.;
            nodalWeightsP = 0.; nodalWeightsN = 0.;
   
            // compute min-/max-values and the fluctuation for subcells
            for (m = 0; m < dofs.numSubcells; m++)
            {
               xMinSubcell(m) = numeric_limits<double>::infinity();
               xMaxSubcell(m) = -xMinSubcell(m);
               fluct = xSum = 0.;
               
               if (exec_mode == 1)
               {
                  asmbl.ComputeSubcellWeights(k, m);
               }
               
               for (i = 0; i < dofs.numDofsSubcell; i++)
               {
                  dofInd = k*nd + dofs.Sub2Ind(m, i);
                  fluct += asmbl.SubcellWeights(k,m,i) * x(dofInd);
                  xMaxSubcell(m) = max(xMaxSubcell(m), x(dofInd));
                  xMinSubcell(m) = min(xMinSubcell(m), x(dofInd));
                  xSum += x(dofInd);
               }
               sumWeightsSubcellP(m) = dofs.numDofsSubcell
                                       * xMaxSubcell(m) - xSum + eps;
               sumWeightsSubcellN(m) = dofs.numDofsSubcell
                                       * xMinSubcell(m) - xSum - eps;

               fluctSubcellP(m) = max(0., fluct);
               fluctSubcellN(m) = min(0., fluct);
               sumFluctSubcellP += fluctSubcellP(m);
               sumFluctSubcellN += fluctSubcellN(m);
            }

            for (m = 0; m < dofs.numSubcells; m++)
            {
               for (i = 0; i < dofs.numDofsSubcell; i++)
               {
                  loc = dofs.Sub2Ind(m, i);
                  dofInd = k*nd + loc;
                  nodalWeightsP(loc) += fluctSubcellP(m)
                                        * ((xMaxSubcell(m) - x(dofInd))
                                        / sumWeightsSubcellP(m)); // eq. (10)
                  nodalWeightsN(loc) += fluctSubcellN(m)
                                        * ((xMinSubcell(m) - x(dofInd))
                                        / sumWeightsSubcellN(m)); // eq. (11)
               }
            }
         }

         for (i = 0; i < nd; i++)
         {
            dofInd = k*nd+i;
            weightP = (dofs.xe_max(k) - x(dofInd)) / sumWeightsP;
            weightN = (dofs.xe_min(k) - x(dofInd)) / sumWeightsN;

            if (lom.OptScheme)
            {
               aux = gamma / (rhoP + eps);
               weightP *= 1. - min(aux * sumFluctSubcellP, 1.);
               weightP += min(aux, 1./(sumFluctSubcellP+eps))*nodalWeightsP(i);

               aux = gamma / (rhoN - eps);
               weightN *= 1. - min(aux * sumFluctSubcellN, 1.);
               weightN += max(aux, 1./(sumFluctSubcellN-eps))*nodalWeightsN(i);
            }

            for (j = 0; j < nd; j++)
            {
               dofInd2 = k*nd+j;
               if (z(dofInd2) > eps)
               {
                  y(dofInd) += weightP * z(dofInd2);
               }
               else if (z(dofInd2) < -eps)
               {
                  y(dofInd) += weightN * z(dofInd2);
               }
            }
            y(dofInd) /= lumpedM(dofInd);
         }
      }
   }
}

// No monotonicity treatment, straightforward high-order scheme
// ydot = M^{-1} (K x + b).
void FE_Evolution::ComputeHighOrderSolution(const Vector &x, Vector &y) const
{
   const FiniteElement* dummy = lom.fes->GetFE(0);
   int i, k, nd = dummy->GetDof(), ne = lom.fes->GetNE();
   Vector alpha(nd); alpha = 1.;
   
   K.Mult(x, z);
   z += b;

   // Incorporate flux terms only if the low order scheme is PDU, RD, or RDS.
   if ((lom.MonoType != DiscUpw_FCT) || (lom.OptScheme))
   {
      // The boundary contributions have been computed in the low order scheme.
      for (k = 0; k < ne; k++)
      {
         for (i = 0; i < dofs.numBdrs; i++)
         {
            LinearFluxLumping(k, nd, i, x, z, alpha);
         }
      }
   }
   
   NeumannSolve(z, y);
}

// High order reconstruction that yields an updated admissible solution by means
// of clipping the solution coefficients within certain bounds and scaling the
// antidiffusive fluxes in a way that leads to local conservation of mass. yH,
// yL are the high and low order discrete time derivatives.
void FE_Evolution::ComputeFCTSolution(const Vector &x, const Vector &yH,
                                      const Vector &yL, Vector &y) const
{
   int j, k, nd, dofInd;
   double sumPos, sumNeg, eps = 1.E-15;
   Vector uClipped, fClipped;

   // Monotonicity terms
   for (k = 0; k < lom.fes->GetMesh()->GetNE(); k++)
   {
      const FiniteElement* el = lom.fes->GetFE(k);
      nd = el->GetDof();

      uClipped.SetSize(nd); uClipped = 0.;
      fClipped.SetSize(nd); fClipped = 0.;
      sumPos = sumNeg = 0.;

      for (j = 0; j < nd; j++)
      {
         dofInd = k*nd+j;
         
         // Compute the bounds for each dof inside the loop.
         dofs.ComputeVertexBounds(x, dofInd);
         
         uClipped(j) = min( dofs.xi_max(dofInd), max( x(dofInd) + dt*yH(dofInd),
                                 dofs.xi_min(dofInd) ) );

         fClipped(j) = lumpedM(dofInd) / dt
                        * ( uClipped(j) - (x(dofInd) + dt * yL(dofInd)) );

         sumPos += max(fClipped(j), 0.);
         sumNeg += min(fClipped(j), 0.);
      }

      for (j = 0; j < nd; j++)
      {
         if ((sumPos + sumNeg > eps) && (fClipped(j) > eps))
         {
            fClipped(j) *= - sumNeg / sumPos;
         }
         if ((sumPos + sumNeg < -eps) && (fClipped(j) < -eps))
         {
            fClipped(j) *= - sumPos / sumNeg;
         }

         // Set y to the discrete time derivative featuring the high order anti-
         // diffusive reconstruction that leads to an forward Euler updated 
         // admissible solution. 
         dofInd = k*nd+j;
         y(dofInd) = yL(dofInd) + fClipped(j) / lumpedM(dofInd);
      }
   }
}


// Implementation of class FE_Evolution
FE_Evolution::FE_Evolution(BilinearForm &Mbf_, SparseMatrix &_M,
                           BilinearForm &_ml, Vector &_lumpedM,
                           BilinearForm &Kbf_, SparseMatrix &_K,
                           const Vector &_b, GridFunction &mpos,
                           GridFunction &vpos, Assembly &_asmbl,
                           LowOrderMethod &_lom, DofInfo &_dofs) :
   TimeDependentOperator(_M.Size()), z(_M.Size()), Mbf(Mbf_), M(_M), ml(_ml),
   lumpedM(_lumpedM), Kbf(Kbf_), K(_K), b(_b), start_pos(mpos.Size()),
   mesh_pos(mpos), vel_pos(vpos), asmbl(_asmbl), lom(_lom), dofs(_dofs) { }

void FE_Evolution::Mult(const Vector &x, Vector &y) const
{
   Mesh *mesh = lom.fes->GetMesh();
   int i, k, dim = mesh->Dimension(), ne = lom.fes->GetNE();
   Array <int> bdrs, orientation;
   FaceElementTransformations *Trans;
   
   // Move towards x0 with current t.
   const double t = GetTime();

   if (exec_mode == 1)
   {
      add(start_pos, t, vel_pos, mesh_pos);
   }

   // Reassemble on the new mesh (given by mesh_pos).
   if (exec_mode == 1)
   {
      ///////////////////////////
      // Element contributions //
      ///////////////////////////
      Mbf.BilinearForm::operator=(0.0);
      Mbf.Assemble();
      Kbf.BilinearForm::operator=(0.0);
      Kbf.Assemble(0);
      ml.BilinearForm::operator=(0.0);
      ml.Assemble();
      ml.SpMat().GetDiag(lumpedM);
      
      ////////////////////////////
      // Boundary contributions //
      ////////////////////////////
      const bool NeedBdr = lom.OptScheme || ( (lom.MonoType != DiscUpw)
                                       && (lom.MonoType != DiscUpw_FCT) );
      
      if (NeedBdr)
      {
         asmbl.bdrInt = 0.;
         for (k = 0; k < ne; k++)
         {
            if (dim==1)
            {
               mesh->GetElementVertices(k, bdrs);
            }
            else if (dim==2)
            {
               mesh->GetElementEdges(k, bdrs, orientation);
            }
            else if (dim==3)
            {
               mesh->GetElementFaces(k, bdrs, orientation);
            }
            
            for (i = 0; i < dofs.numBdrs; i++)
            {
               Trans = mesh->GetFaceElementTransformations(bdrs[i]);
               asmbl.ComputeFluxTerms(k, i, Trans, lom);
            }
         }
      }
   }

   if (lom.MonoType == 0)
   {
      ComputeHighOrderSolution(x, y);
   }
   else
   {
      if (lom.MonoType % 2 == 1)
      {
         ComputeLowOrderSolution(x, y);
      }
      else if (lom.MonoType % 2 == 0)
      {
         Vector yH, yL;
         yH.SetSize(x.Size()); yL.SetSize(x.Size());

         ComputeLowOrderSolution(x, yL);
         ComputeHighOrderSolution(x, yH);
         ComputeFCTSolution(x, yH, yL, y);
      }
   }
}

#ifdef USE_LUA
void lua_velocity_function(const Vector &x, Vector &v)
{
   lua_getglobal(L, "velocity_function");
   int dim = x.Size();
   
   lua_pushnumber(L, x(0));
   if (dim > 1)
      lua_pushnumber(L, x(1));
   if (dim > 2)
      lua_pushnumber(L, x(2));

   double v0 = 0;
   double v1 = 0;
   double v2 = 0;
   lua_call(L, dim, dim);
   v0 = (double)lua_tonumber(L, -1);
   lua_pop(L, 1);
   if (dim > 1) {
      v1 = (double)lua_tonumber(L, -1);
      lua_pop(L, 1);
   }
   if (dim > 2) {
      v2 = (double)lua_tonumber(L, -1);
      lua_pop(L, 1);
   }

   v(0) = v0;
   if (dim > 1) {
     v(0) = v1;
     v(1) = v0;
   }
   if (dim > 2) {
      v(0) = v2;
      v(1) = v1;
      v(2) = v0;
   }
}
#endif

// Velocity coefficient
void velocity_function(const Vector &x, Vector &v)
{
#ifdef USE_LUA
   lua_velocity_function(x, v);
   return;
#endif
  
   int dim = x.Size();

   // map to the reference [-1,1] domain
   Vector X(dim);
   for (int i = 0; i < dim; i++)
   {
      double center = (bb_min[i] + bb_max[i]) * 0.5;
      X(i) = 2 * (x(i) - center) / (bb_max[i] - bb_min[i]);
   }
   
   int ProbExec = problem_num % 20;

   switch (ProbExec)
   {
      case 0:
      {
         // Translations in 1D, 2D, and 3D
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = sqrt(2./3.); v(1) = sqrt(1./3.); break;
            case 3: v(0) = sqrt(3./6.); v(1) = sqrt(2./6.); v(2) = sqrt(1./6.);
               break;
         }
         break;
      }
      case 1:
      case 2:
      case 4:
      {
         // Clockwise rotation in 2D around the origin
         const double w = M_PI/2;
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = -w*X(1); v(1) = w*X(0); break;
            case 3: v(0) = -w*X(1); v(1) = w*X(0); v(2) = 0.0; break;
         }
         break;
      }
      case 3:
      {
         // Clockwise twisting rotation in 2D around the origin
         const double w = M_PI/2;
         double d = max((X(0)+1.)*(1.-X(0)),0.) * max((X(1)+1.)*(1.-X(1)),0.);
         d = d*d;
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = d*w*X(1); v(1) = -d*w*X(0); break;
            case 3: v(0) = d*w*X(1); v(1) = -d*w*X(0); v(2) = 0.0; break;
         }
         break;
      }
      case 5:
      {
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = 1.0; v(1) = 1.0; break;
            case 3: v(0) = 1.0; v(1) = 1.0; v(2) = 1.0; break;
         }
         break;
      }
      case 10:
      case 11:
      case 12:
      case 13:
      case 14:
      case 15:
      {
         // Taylor-Green velocity, used for mesh motion in remap tests used for
         // all possible initial conditions.

         // Map [-1,1] to [0,1].
         for (int d = 0; d < dim; d++) { X(d) = X(d) * 0.5 + 0.5; }

         if (dim == 1) { MFEM_ABORT("Not implemented."); }
         v(0) =  sin(M_PI*X(0)) * cos(M_PI*X(1));
         v(1) = -cos(M_PI*X(0)) * sin(M_PI*X(1));
         if (dim == 3)
         {
            v(0) *= cos(M_PI*X(2));
            v(1) *= cos(M_PI*X(2));
            v(2) = 0.0;
         }
         break;
      }
   }
}

double box(std::pair<double,double> p1, std::pair<double,double> p2,
           double theta,
           std::pair<double,double> origin, double x, double y)
{
   double xmin=p1.first;
   double xmax=p2.first;
   double ymin=p1.second;
   double ymax=p2.second;
   double ox=origin.first;
   double oy=origin.second;

   double pi = M_PI;
   double s=std::sin(theta*pi/180);
   double c=std::cos(theta*pi/180);

   double xn=c*(x-ox)-s*(y-oy)+ox;
   double yn=s*(x-ox)+c*(y-oy)+oy;

   if (xn>xmin && xn<xmax && yn>ymin && yn<ymax)
   {
      return 1.0;
   }
   else
   {
      return 0.0;
   }
}

double box3D(double xmin, double xmax, double ymin, double ymax, double zmin,
             double zmax, double theta, double ox, double oy, double x,
             double y, double z)
{
   double pi = M_PI;
   double s=std::sin(theta*pi/180);
   double c=std::cos(theta*pi/180);

   double xn=c*(x-ox)-s*(y-oy)+ox;
   double yn=s*(x-ox)+c*(y-oy)+oy;

   if (xn>xmin && xn<xmax && yn>ymin && yn<ymax && z>zmin && z<zmax)
   {
      return 1.0;
   }
   else
   {
      return 0.0;
   }
}

double get_cross(double rect1, double rect2)
{
   double intersection=rect1*rect2;
   return rect1+rect2-intersection; //union
}

double ring(double rin, double rout, Vector c, Vector y)
{
   double r = 0.;
   int dim = c.Size();
   if (dim != y.Size())
   {
      mfem_error("Origin vector and variable have to be of the same size.");
   }
   for (int i = 0; i < dim; i++)
   {
      r += pow(y(i)-c(i), 2.);
   }
   r = sqrt(r);
   if (r>rin && r<rout)
   {
      return 1.0;
   }
   else
   {
      return 0.0;
   }
}

// Initial condition as defined by lua function
#ifdef USE_LUA
double lua_u0_function(const Vector &x)
{
   lua_getglobal(L, "initial_function");
   int dim = x.Size();

   lua_pushnumber(L, x(0));
   if (dim > 1)
      lua_pushnumber(L, x(1));
   if (dim > 2)
      lua_pushnumber(L, x(2));

   lua_call(L, dim, 1);
   double u = (double)lua_tonumber(L, -1);
   lua_pop(L, 1);

   return u;
}
#endif

// Initial condition: lua function or hardcoded functions
double u0_function(const Vector &x)
{
#ifdef USE_LUA
   return lua_u0_function(x);
#endif
   
   int dim = x.Size();

   // map to the reference [-1,1] domain
   Vector X(dim);
   for (int i = 0; i < dim; i++)
   {
      double center = (bb_min[i] + bb_max[i]) * 0.5;
      X(i) = 2 * (x(i) - center) / (bb_max[i] - bb_min[i]);
   }
   
   int ProbExec = problem_num % 10;

   switch (ProbExec)
   {
      case 0:
      case 1:
      {
         switch (dim)
         {
            case 1:
               return exp(-40.*pow(X(0)-0.5,2));
            case 2:
            case 3:
            {
               double rx = 0.45, ry = 0.25, cx = 0., cy = -0.2, w = 10.;
               if (dim == 3)
               {
                  const double s = (1. + 0.25*cos(2*M_PI*X(2)));
                  rx *= s;
                  ry *= s;
               }
               return ( erfc(w*(X(0)-cx-rx))*erfc(-w*(X(0)-cx+rx)) *
                        erfc(w*(X(1)-cy-ry))*erfc(-w*(X(1)-cy+ry)) )/16;
            }
         }
      }
      case 2:
      {
         double x_ = X(0), y_ = X(1), rho, phi;
         rho = hypot(x_, y_);
         phi = atan2(y_, x_);
         return pow(sin(M_PI*rho),2)*sin(3*phi);
      }
      case 3:
      {
         const double f = M_PI;
         return .5*(sin(f*X(0))*sin(f*X(1)) + 1.); // modified by Hennes
      }
      case 4:
      {
         double scale = 0.0225;
         double coef = (0.5/sqrt(scale));
         double slit = (X(0) <= -0.05) || (X(0) >= 0.05) || (X(1) >= 0.7);
         double cone = coef * sqrt(pow(X(0), 2.) + pow(X(1) + 0.5, 2.));
         double hump = coef * sqrt(pow(X(0) + 0.5, 2.) + pow(X(1), 2.));

         return (slit && ((pow(X(0),2.) + pow(X(1)-.5,2.))<=4.*scale)) ? 1. : 0.
                + (1. - cone) * (pow(X(0), 2.) + pow(X(1)+.5, 2.) <= 4.*scale)
                + .25 * (1. + cos(M_PI*hump))
                       * ((pow(X(0)+.5, 2.) + pow(X(1), 2.)) <= 4.*scale);
      }
      case 5:
      {
         Vector y(dim);
         for (int i = 0; i < dim; i++) { y(i) = 50. * (x(i) + 1.); }

         if (dim==1)
         {
            mfem_error("This test is not supported in 1D.");
         }
         else if (dim==2)
         {
            std::pair<double, double> p1;
            std::pair<double, double> p2;
            std::pair<double, double> origin;

            // cross
            p1.first=14.; p1.second=3.;
            p2.first=17.; p2.second=26.;
            origin.first = 15.5;
            origin.second = 11.5;
            double rect1=box(p1,p2,-45.,origin,y(0),y(1));
            p1.first=7.; p1.second=10.;
            p2.first=32.; p2.second=13.;
            double rect2=box(p1,p2,-45.,origin,y(0),y(1));
            double cross=get_cross(rect1,rect2);
            // rings
            Vector c(dim);
            c(0) = 40.; c(1) = 40;
            double ring1 = ring(7., 10., c, y);
            c(1) = 20.;
            double ring2 = ring(3., 7., c, y);

            return cross + ring1 + ring2;
         }
         else
         {
            // cross
            double rect1 = box3D(7.,32.,10.,13.,10.,13.,-45.,15.5,11.5,
                                 y(0),y(1),y(2));
            double rect2 = box3D(14.,17.,3.,26.,10.,13.,-45.,15.5,11.5,
                                 y(0),y(1),y(2));
            double rect3 = box3D(14.,17.,10.,13.,3.,26.,-45.,15.5,11.5,
                                 y(0),y(1),y(2));

            double cross = get_cross(get_cross(rect1, rect2), rect3);

            // rings
            Vector c1(dim), c2(dim);
            c1(0) = 40.; c1(1) = 40; c1(2) = 40.;
            c2(0) = 40.; c2(1) = 20; c2(2) = 20.;

            double shell1 = ring(7., 10., c1, y);
            double shell2 = ring(3., 7., c2, y);

            double dom2 = cross + shell1 + shell2;

            // cross
            rect1 = box3D(2.,27.,30.,33.,30.,33.,0.,0.,0.,y(0),y(1),y(2));
            rect2 = box3D(9.,12.,23.,46.,30.,33.,0.,0.,0.,y(0),y(1),y(2));
            rect3 = box3D(9.,12.,30.,33.,23.,46.,0.,0.,0.,y(0),y(1),y(2));

            cross = get_cross(get_cross(rect1, rect2), rect3);

            double ball1 = ring(0., 7., c1, y);
            double ball2 = ring(0., 3., c2, y);
            double shell3 = ring(7., 10., c2, y);

            double dom3 = cross + ball1 + ball2 + shell3;

            double dom1 = 1. - get_cross(dom2, dom3);

            return dom1 + 2.*dom2 + 3.*dom3;
         }
      }
   }
   return 0.0;
}

#ifdef USE_LUA
double lua_inflow_function(const Vector& x)
{
   lua_getglobal(L, "boundary_condition");

   int dim = x.Size();

   double t;
   adv ? t = adv->GetTime() : t = 0.0;
   
   for (int d = 0; d < dim; d++) {
      lua_pushnumber(L, x(d));
   }
   lua_pushnumber(L, t);

   lua_call(L, dim+1, 1);
   double u = (double)lua_tonumber(L, -1);
   lua_pop(L, 1);

   return u;
}
#endif

// Inflow boundary condition (zero for the problems considered in this example)
double inflow_function(const Vector &x)
{
#ifdef USE_LUA
   return lua_inflow_function(x);
#endif
   
   switch (problem_num)
   {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4:
      case 5: return 0.0;
   }
   return 0.0;
}