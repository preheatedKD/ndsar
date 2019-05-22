
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <queue>

#include "NDArray.h"
#include "Eigen/Core"
#include "Eigen/Eigenvalues"

typedef std::complex<float> clx;

using Eigen::RowMajor;
//using Eigen::Ref;

template <int N>
class DistanceBase {
  public:
    virtual float compute(const Eigen::Matrix<clx, N, N, RowMajor> &A, const Eigen::Matrix<clx, N, N, RowMajor> &B) = 0;
};

template <int N>
class DistanceAI : public DistanceBase<N> {
  Eigen::GeneralizedSelfAdjointEigenSolver<Eigen::Matrix<clx, N, N, RowMajor> > es;
  public:
    float compute(const Eigen::Matrix<clx, N, N, RowMajor> &A, const Eigen::Matrix<clx, N, N, RowMajor> &B)
    {
      es.compute(A, B, Eigen::EigenvaluesOnly);
      return es.eigenvalues().array().log().abs2().sum();
    }
};

template <int N>
class DistanceEu : public DistanceBase<N> {
  public:
    float compute(const Eigen::Matrix<clx, N, N, RowMajor> &A, const Eigen::Matrix<clx, N, N, RowMajor> &B)
    {
      return (A - B).squaredNorm();
    }
};

template <int N>
class DistanceLogDiag : public DistanceBase<N> {
  public:
    float compute(const Eigen::Matrix<clx, N, N, RowMajor> &A, const Eigen::Matrix<clx, N, N, RowMajor> &B)
    {
      return (A.diagonal().real().array().log() -
          B.diagonal().real().array().log())
        .array().abs2().sum();
    }
};


// Bilateral filter with different similarities
template<int N>
void ndsar_blf(const NDArray<clx> &img, NDArray<clx> &imgout,
    float gammaS = 2.8, float gammaR = 1.33,
    bool TRICK=true, bool FLATW=false, int METHOD = 1, int d=0)
{

  using namespace Eigen;

  typedef Eigen::Matrix<clx, N, N, RowMajor> EigMat;
  typedef Eigen::Map<const EigMat> ConstEigMap;
  typedef Eigen::Map<EigMat> EigMap;
  typedef Eigen::Matrix<float, N, 1> EigVec;

  float gr2 = gammaR * gammaR;
  float gs2 = gammaS * gammaS;

   // Calculating the window size for the gaussian weights
  int H = ceil(std::sqrt(3.0)*gammaS);

  int dimaz = img.dimI(), dimrg = img.dimJ(), dimmat = img.dimK();

  NDArray<float> weights(dimaz, dimrg, 1, 1, 0.0);

  // precomputing spatial Gaussian weights
  NDArray<float> WIm(2*H+1, 2*H+1, 1, 1, 1.0);
  if(!FLATW) {
    arr_forIJ(WIm, i, j) {
      int s = i - H;
      int t = j - H;
      WIm(i, j) = std::exp(- (s*s + t*t) / gs2 );
    }
  }
  else {
    arr_forIJ(WIm, i, j) {
      int s = i - H;
      int t = j - H;
      if((s*s + t*t) <= (H+1)*(H+1)) WIm(i, j) = 1.0;
      else WIm(i, j) = 0.0;
    }
  }

  NDArray<clx> img0(dimaz, dimrg, dimmat, dimmat, 0.0);

  if(METHOD == 1 || METHOD == 3) { // Affine-Invariant & Log Diagonal
    arr_forIJKL(img, i, j, k, l) {
      img0(i, j, k, l) = img(i, j, k, l);
    }
  }

  if(METHOD == 2) {// Log-euclidean -> pre-compute logs
    SelfAdjointEigenSolver<Matrix<clx, N, N, RowMajor> > es0(d);
    ConstEigMap Tp(NULL, d, d);
    EigMat V(d, d), Ts(d, d);
    EigVec M(d);
    arr_forIJ(img, i, j) {
      new (&Tp) ConstEigMap(&img(i, j),d,d);
      es0.compute(Tp);
      V = es0.eigenvectors();
      M = es0.eigenvalues().array().log();
      Ts = V * M.asDiagonal() * V.adjoint();
      EigMap(&img0(i, j), d, d) = Ts;
    }
  }

  EigMat Ts(d, d); // filtered pixel
  ConstEigMap T0(NULL, d, d), Ti(NULL, d, d), T0b(NULL, d, d), Tib(NULL, d, d); // pointers to pixel 0 and i

  static DistanceBase<N>* dist_cur;
  #pragma omp threadprivate(dist_cur)

  #pragma omp parallel
  switch(METHOD) {
    case 1:
    dist_cur = new DistanceAI<N>;
    break;
    case 2:
    dist_cur = new DistanceEu<N>;
    break;
    case 3:
    dist_cur = new DistanceLogDiag<N>;
    break;
    default:
      dist_cur = new DistanceAI<N>;
      break;
  }

  #pragma omp parallel for firstprivate(Ts, T0, Ti, T0b, Tib)
  arr_forIJ(img,i,j) {
    float SumWeight = 0;
    Ts.setZero();
    new (&T0) ConstEigMap(&img0(i, j), d, d); // using Eigen dynamic api in all cases
    new (&T0b) ConstEigMap(&img(i, j), d, d);

    const int smin = std::max(i-H, 0),
          smax = std::min(i+H, img.dimI()-1),
          tmin = std::max(j-H, 0),
          tmax = std::min(j+H, img.dimJ()-1);

    float WMax = 0.0;
    for(int s = smin; s <= smax; ++s)
      for(int t = tmin; t <= tmax; ++t) {
        if(s != i || t != j) {
          new (&Ti) ConstEigMap(&img0(s,t), d, d);
          new (&Tib) ConstEigMap(&img(s,t), d, d);
          const float D = dist_cur->compute(T0, Ti);
          float W = 0.0;
          if(!std::isnan(D)) W = std::exp(- D/gr2) * WIm(s-i+H, t-j+H);

          Ts += W*Tib;
          SumWeight += W;
          if(W > WMax) WMax = W;
        }
      }
    // Treating central pixel separately
    if(TRICK) {
      Ts += WMax*T0b;
      SumWeight += WMax;
    }
    else {
      Ts += T0b;
      SumWeight += 1.0;
    }
    if(SumWeight > 1.0e-10) Ts /= SumWeight;
    else Ts = T0b;

    weights(i, j) = SumWeight;
    EigMap(&imgout(i, j), d, d) = Ts;
  }

}

// NLM filter with different similarities
template<int N>
void ndsar_nlm(const NDArray<clx> &img, NDArray<clx> &imgout,
    float gammaS = 2.8, float gammaR = 1.33, int Psiz = 3,
    bool TRICK=true, bool FLATW=false, int METHOD = 1, int d=0)
{

  using namespace Eigen;

  typedef Eigen::Matrix<clx, N, N, RowMajor> EigMat;
  typedef Eigen::Map<const EigMat> ConstEigMap;
  typedef Eigen::Map<EigMat> EigMap;
  typedef Eigen::Matrix<float, N, 1> EigVec;

  float gr2 = gammaR * gammaR;
  float gs2 = gammaS * gammaS;

   // Calculating the window size for the gaussian weights
  int H = ceil(std::sqrt(3.0)*gammaS);
  int PH = Psiz/2;

  int dimaz = img.dimI(), dimrg = img.dimJ(), dimmat = img.dimK();
  NDArray<float> weights(dimaz, dimrg, 1, 1, 0.0);

  // precomputing spatial Gaussian weights
  NDArray<float> WIm(2*H+1, 2*H+1, 1, 1, 1.0);
  if(!FLATW) {
    arr_forIJ(WIm, i, j) {
      int s = i - H;
      int t = j - H;
      WIm(i, j) = std::exp(- (s*s + t*t) / gs2 );
    }
  }
  else {
    arr_forIJ(WIm, i, j) {
      int s = i - H;
      int t = j - H;
      if((s*s + t*t) <= (H+1)*(H+1)) WIm(i, j) = 1.0;
      else WIm(i, j) = 0.0;
    }
  }

  NDArray<clx> img0(dimaz, dimrg, dimmat, dimmat, 0.0);

  if(METHOD == 1 || METHOD == 3) { // Affine-Invariant & Log Diagonal
    arr_forIJKL(img, i, j, k, l) {
      img0(i, j, k, l) = img(i, j, k, l);
    }
  }

  if(METHOD == 2) {// Log-euclidean -> pre-compute logs
    SelfAdjointEigenSolver<Matrix<clx, N, N, RowMajor> > es0(d);
    ConstEigMap Tp(NULL, d, d);
    EigMat V(d, d), Ts(d, d);
    EigVec M(d);
    arr_forIJ(img, i, j) {
      new (&Tp) ConstEigMap(&img(i, j),d,d);
      es0.compute(Tp);
      V = es0.eigenvectors();
      M = es0.eigenvalues().array().log();
      Ts = V * M.asDiagonal() * V.adjoint();
      EigMap(&img0(i, j), d, d) = Ts;
    }
  }

  EigMat Ts(d, d); // filtered pixel
  ConstEigMap T0(NULL, d, d), Ti(NULL, d, d), T0w(NULL, d, d), Tiw(NULL, d, d); // pointers to pixel 0 and i

  static DistanceBase<N>* dist_cur;
  #pragma omp threadprivate(dist_cur)

  #pragma omp parallel
  switch(METHOD) {
    case 1:
      dist_cur = new DistanceAI<N>;
      break;
    case 2:
      dist_cur = new DistanceEu<N>;
      break;
    case 3:
      dist_cur = new DistanceLogDiag<N>;
      break;
    default:
      dist_cur = new DistanceAI<N>;
      break;
  }

  #pragma omp parallel for firstprivate(Ts, T0, Ti, T0w, Tiw)
  arr_forIJ(img, i, j) {
    float SumWeight = 0;
    Ts.setZero();

    // using Eigen dynamic api in all cases
    new (&T0) ConstEigMap(&img(i, j), d, d); // data to filter

    const int smin = std::max(i-H, 0),
          smax = std::min(i+H, dimaz-1),
          tmin = std::max(j-H, 0),
          tmax = std::min(j+H, dimrg-1);

    float WMax = 0.0;
    for(int s = smin; s <= smax; ++s)
      for(int t = tmin; t <= tmax; ++t) {
        if(s != i || t != j) {
          new (&Ti) ConstEigMap(&img(s, t), d, d);
          const int umin = (i - PH < 0 || s - PH < 0)?-std::min(i, s):-PH,
                    vmin = (j - PH < 0 || t - PH < 0)?-std::min(j, t):-PH,
                    umax = (i + PH > dimaz - 1 || s + PH > dimaz - 1)?std::min(dimaz-1-i, dimaz-1-s):PH,
                    vmax = (j + PH > dimrg - 1 || t + PH > dimrg - 1)?std::min(dimrg-1-j, dimaz-1-t):PH;
          float W = 0.0,
                D = 0.0; // init weight and distance
          const int cnt = (umax-umin+1)*(vmax-vmin+1);
          for(int u = umin; u <= umax; ++u) // loop on patch
            for(int v = vmin; v <= vmax; ++v) {
              new (&T0w) ConstEigMap(&img0(i+u, j+v), d, d); // used to compute weights
              new (&Tiw) ConstEigMap(&img0(s+u, t+v), d, d);
              D += dist_cur->compute(T0w, Tiw);
            }
          if(!std::isnan(D)) W = std::exp(- D/(gr2*cnt)) * WIm(s-i+H, t-j+H);
          Ts += W*Ti;
          SumWeight += W;
          if(W > WMax){
            WMax = W;
          }
        }
      }

    // Treating central pixel separately
    if(TRICK) {
      Ts += WMax*T0;
      SumWeight += WMax;
    }
    else {
      Ts += T0;
      SumWeight += 1.0;
    }
    if(SumWeight > 1.0e-10) Ts /= SumWeight;
    else Ts = T0;

    weights(i, j) = SumWeight;
    EigMap(&imgout(i, j), d, d) = Ts;
  }

}

// NLM for single channel (all distances are equivalent)
template<>
void ndsar_nlm<1>(const NDArray<clx> &img, NDArray<clx> &imgout,
    float gammaS, float gammaR, int Psiz,
    bool TRICK, bool FLATW, int METHOD, int d)
{

  float gr2 = gammaR * gammaR;
  float gs2 = gammaS * gammaS;

   // Calculating the window size for the gaussian weights
  int H = ceil(std::sqrt(3.0)*gammaS);
  int PH = Psiz/2;
  int dimaz = img.dimI(), dimrg = img.dimJ();
  NDArray<float> weights(dimaz, dimrg, 1, 1, 0.0);


  // precomputing spatial Gaussian weights
  NDArray<float> WIm(2*H+1, 2*H+1, 1, 1, 1.0);
  NDArray<float> Wdisp(2*H+1, 2*H+1, 1, 1, 0.0);
  if(!FLATW) {
    arr_forIJ(WIm, i, j) {
      int s = i - H;
      int t = j - H;
      WIm(i, j) = std::exp(- (s*s + t*t) / gs2 );
    }
  }
  else {
    arr_forIJ(WIm, i, j) {
      int s = i - H;
      int t = j - H;
      if((s*s + t*t) <= (H+1)*(H+1)) WIm(i, j) = 1.0;
      else WIm(i, j) = 0.0;
    }
  }

  NDArray<float> img0(img.dimI(), img.dimJ(), img.dimK(), img.dimL(), 0.0);
  arr_forIJ(img0, i, j)
    img0(i, j) = logf(real(img(i, j)));

  clx res;
  #pragma omp parallel for firstprivate(res)
  arr_forIJ(img, i, j) {
    float SumWeight = 0;
    res = 0.0;

    const int smin = std::max(i-H, 0),
          smax = std::min(i+H, dimaz-1),
          tmin = std::max(j-H, 0),
          tmax = std::min(j+H, dimrg-1);

    float WMax = 0.0;
    for(int s = smin; s <= smax; ++s)
      for(int t = tmin; t <= tmax; ++t) {
        if(s != i || t != j) {
          const int umin = (i - PH < 0 || s - PH < 0)?-std::min(i, s):-PH,
                    vmin = (j - PH < 0 || t - PH < 0)?-std::min(j, t):-PH,
                    umax = (i + PH > dimaz - 1 || s + PH > dimaz - 1)?std::min(dimaz-1-i, dimaz-1-s):PH,
                    vmax = (j + PH > dimrg - 1 || t + PH > dimrg - 1)?std::min(dimrg-1-j, dimrg-1-t):PH;
          float W = 0.0, D = 0.0; // init weight and distance
          const int cnt = (umax-umin+1)*(vmax-vmin+1);
          for(int u = umin; u <= umax; ++u) // loop on patch
            for(int v = vmin; v <= vmax; ++v) {
              const float diff = img0(i+u,j+v) - img0(s+u,t+v);
              D += diff*diff;
              if(W > WMax){
               WMax = W;
              }
            }

          if(!std::isnan(D)) W = std::exp(- D/(gr2*cnt)) * WIm(s-i+H, t-j+H);
          res += W*img(s, t);
          SumWeight += W;
          if(W > WMax) WMax = W;

        }
      }

    // Treating central pixel separately
    if(TRICK) {
      res += WMax*img(i,j);
      SumWeight += WMax;
    }
    else {
      res += img(i, j);
      SumWeight += 1.0;
    }

    // final pixel value
    if(SumWeight > 1e-10) imgout(i, j) = res / SumWeight;
    else  imgout(i, j) = img(i, j);

  }

}

// BLF for single channel (all distances are equivalent)
template<>
void ndsar_blf<1>(const NDArray<clx> &img, NDArray<clx> &imgout,
    float gammaS, float gammaR,
    bool TRICK, bool FLATW, int METHOD, int d)
{

  float gr2 = gammaR * gammaR;
  float gs2 = gammaS * gammaS;

   // Calculating the window size for the gaussian weights
  int H = ceil(std::sqrt(3.0)*gammaS);
  int dimaz = img.dimI(), dimrg = img.dimJ();

  // precomputing spatial Gaussian weights
  NDArray<float> WIm(2*H+1, 2*H+1, 1, 1, 1.0);
  if(!FLATW) {
    arr_forIJ(WIm, i, j) {
      int s = i - H;
      int t = j - H;
      WIm(i, j) = std::exp(- (s*s + t*t) / gs2 );
    }
  }
  else {
    arr_forIJ(WIm, i, j) {
      int s = i - H;
      int t = j - H;
      if((s*s + t*t) <= (H+1)*(H+1)) WIm(i, j) = 1.0;
      else WIm(i, j) = 0.0;
    }
  }

  NDArray<float> img0(img.dimI(), img.dimJ(), img.dimK(), img.dimL(), 0.0);
  arr_forIJ(img0, i, j)
    img0(i, j) = logf(real(img(i, j)));

  NDArray<float> weights(dimaz, dimrg, 1, 1, 0.0);

  clx res;
  #pragma omp parallel for firstprivate(res)
  arr_forIJ(img, i, j) {
    float SumWeight = 0;
    res = 0.0;

    const int smin = std::max(i-H, 0),
          smax = std::min(i+H, dimaz-1),
          tmin = std::max(j-H, 0),
          tmax = std::min(j+H, dimrg-1);

    float WMax = 0.0;
    for(int s = smin; s <= smax; ++s)
      for(int t = tmin; t <= tmax; ++t) {
        if(s != i || t != j) {
          const float D = img0(i, j) - img0(s, t);
          float W = 0.0;
          if(!std::isnan(D)) W = std::exp(- D*D / gr2) * WIm(s-i+H, t-j+H);

          res += W*img(s,t);
          SumWeight += W;
          if(W > WMax) WMax = W;
        }
      }


    // Treating central pixel separately
    if(TRICK) {
      res += WMax*img(i, j);
      SumWeight += WMax;
    }
    else {
      res += img(i, j);
      SumWeight += 1.0;
    }

    // final pixel value
    if(SumWeight > 1.0e-10) imgout(i, j) = res / SumWeight;
    else imgout(i, j) = img(i, j);
  }

}

// ---------------- INTERFACES ---------------------
void ndsar_blf_cpp(clx* data_view, clx* data_view2,
    int* shape, float gs, float gr, bool TRICK, bool FLAT, int METHOD = 1)
{
  int nl = shape[0];
  int nc = shape[1];
  int nlm = shape[2];
  int ncm = shape[3];
  NDArray<clx> img(data_view, nl, nc, nlm, ncm);
  NDArray<clx> imgout(data_view2, nl, nc, nlm, ncm);
  switch(nlm) {
    case 1:
      ndsar_blf<1>(img, imgout, gs, gr, TRICK, FLAT, METHOD);
      break;
   default:
        ndsar_blf<Eigen::Dynamic>(img, imgout, gs, gr, TRICK, FLAT, METHOD, nlm);
      break;
  }
}

void ndsar_nlm_cpp(clx* data_view, clx* data_view2,
    int* shape, float gs, float gr, int Psiz, bool TRICK, bool FLAT, int METHOD = 1)
{
  int nl = shape[0];
  int nc = shape[1];
  int nlm = shape[2];
  int ncm = shape[3];
  NDArray<clx> img(data_view, nl, nc, nlm, ncm);
  NDArray<clx> imgout(data_view2, nl, nc, nlm, ncm);

  switch(nlm) {
    case 1:
      ndsar_nlm<1>(img, imgout, gs, gr, Psiz, TRICK, FLAT, METHOD);
      break;
   default:
        ndsar_nlm<Eigen::Dynamic>(img, imgout, gs, gr, Psiz, TRICK, FLAT, METHOD, nlm);
      break;
  }
}
