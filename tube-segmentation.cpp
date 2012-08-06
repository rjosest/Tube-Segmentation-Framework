#include "tube-segmentation.hpp"
#include "SIPL/Exceptions.hpp"
#include <chrono>
#include <queue>
#include <stack>
#include <list>
#include <cstdio>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/kruskal_min_spanning_tree.hpp>
#include <boost/graph/connected_components.hpp>

template <typename T>
void writeToRaw(T * voxels, std::string filename, int SIZE_X, int SIZE_Y, int SIZE_Z) {
    FILE * file = fopen(filename.c_str(), "wb");
    fwrite(voxels, sizeof(T), SIZE_X*SIZE_Y*SIZE_Z, file);
    fclose(file);
}
template <typename T>
T * readFromRaw(std::string filename, int SIZE_X, int SIZE_Y, int SIZE_Z) {
    FILE * file = fopen(filename.c_str(), "rb");
    T * data = new T[SIZE_X*SIZE_Y*SIZE_Z];
    fread(data, sizeof(T), SIZE_X*SIZE_Y*SIZE_Z, file);
    fclose(file);
    return data;
}

//#define TIMING

#ifdef TIMING
#define INIT_TIMER auto timerStart = std::chrono::high_resolution_clock::now();
#define START_TIMER  timerStart = std::chrono::high_resolution_clock::now();
#define STOP_TIMER(name)  std::cout << "RUNTIME of " << name << ": " << \
        std::chrono::duration_cast<std::chrono::milliseconds>( \
                            std::chrono::high_resolution_clock::now()-timerStart \
                    ).count() << " ms " << std::endl; 
#else
#define INIT_TIMER
#define START_TIMER
#define STOP_TIMER(name)
#endif

using SIPL::float3;
using SIPL::int3;

using namespace cl;

float getParamf(paramList parameters, std::string parameterName, float defaultValue) {
    if(parameters.count(parameterName) == 1) {
        return atof(parameters[parameterName].c_str());
    } else {
        return defaultValue;
    }
}

int getParami(paramList parameters, std::string parameterName, int defaultValue) {
    if(parameters.count(parameterName) == 1) {
        return atoi(parameters[parameterName].c_str());
    } else {
        return defaultValue;
    }
}

std::string getParamstr(paramList parameters, std::string parameterName, std::string defaultValue) {
    if(parameters.count(parameterName) == 1) {
        return parameters[parameterName];
    } else {
        return defaultValue;
    }
}

typedef struct CenterlinePoint {
    int3 pos;
    bool large;
    CenterlinePoint * next;
} CenterlinePoint;

typedef struct point {
    float value;
    int x,y,z;
} point;

class PointComparison {
    public:
    bool operator() (const point &lhs, const point &rhs) const {
        return (lhs.value < rhs.value);
    }
};

bool inBounds(SIPL::int3 pos, SIPL::int3 size) {
    return pos.x > 0 && pos.y > 0 && pos.z > 0 && pos.x < size.x && pos.y < size.y && pos.z < size.z;
}

#define LPOS(a,b,c) (a)+(b)*(size.x)+(c)*(size.x*size.y)
#define M(a,b,c) 1-sqrt(pow(T.Fx[a+b*size.x+c*size.x*size.y],2.0f) + pow(T.Fy[a+b*size.x+c*size.x*size.y],2.0f) + pow(T.Fz[a+b*size.x+c*size.x*size.y],2.0f))
#define SQR_MAG(pos) sqrt(pow(T.Fx[pos.x+pos.y*size.x+pos.z*size.x*size.y],2.0f) + pow(T.Fy[pos.x+pos.y*size.x+pos.z*size.x*size.y],2.0f) + pow(T.Fz[pos.x+pos.y*size.x+pos.z*size.x*size.y],2.0f))

#define SIZE 3

float hypot2(float x, float y) {
  return sqrt(x*x+y*y);
}

// Symmetric Householder reduction to tridiagonal form.

void tred2(float V[SIZE][SIZE], float d[SIZE], float e[SIZE]) {

//  This is derived from the Algol procedures tred2 by
//  Bowdler, Martin, Reinsch, and Wilkinson, Handbook for
//  Auto. Comp., Vol.ii-Linear Algebra, and the corresponding
//  Fortran subroutine in EISPACK.

  for (int j = 0; j < SIZE; j++) {
    d[j] = V[SIZE-1][j];
  }

  // Householder reduction to tridiagonal form.

  for (int i = SIZE-1; i > 0; i--) {

    // Scale to avoid under/overflow.

    float scale = 0.0f;
    float h = 0.0f;
    for (int k = 0; k < i; k++) {
      scale = scale + fabs(d[k]);
    }
    if (scale == 0.0f) {
      e[i] = d[i-1];
      for (int j = 0; j < i; j++) {
        d[j] = V[i-1][j];
        V[i][j] = 0.0f;
        V[j][i] = 0.0f;
      }
    } else {

      // Generate Householder vector.

      for (int k = 0; k < i; k++) {
        d[k] /= scale;
        h += d[k] * d[k];
      }
      float f = d[i-1];
      float g = sqrt(h);
      if (f > 0) {
        g = -g;
      }
      e[i] = scale * g;
      h = h - f * g;
      d[i-1] = f - g;
      for (int j = 0; j < i; j++) {
        e[j] = 0.0f;
      }

      // Apply similarity transformation to remaining columns.

      for (int j = 0; j < i; j++) {
        f = d[j];
        V[j][i] = f;
        g = e[j] + V[j][j] * f;
        for (int k = j+1; k <= i-1; k++) {
          g += V[k][j] * d[k];
          e[k] += V[k][j] * f;
        }
        e[j] = g;
      }
      f = 0.0f;
      for (int j = 0; j < i; j++) {
        e[j] /= h;
        f += e[j] * d[j];
      }
      float hh = f / (h + h);
      for (int j = 0; j < i; j++) {
        e[j] -= hh * d[j];
      }
      for (int j = 0; j < i; j++) {
        f = d[j];
        g = e[j];
        for (int k = j; k <= i-1; k++) {
          V[k][j] -= (f * e[k] + g * d[k]);
        }
        d[j] = V[i-1][j];
        V[i][j] = 0.0f;
      }
    }
    d[i] = h;
  }

  // Accumulate transformations.

  for (int i = 0; i < SIZE-1; i++) {
    V[SIZE-1][i] = V[i][i];
    V[i][i] = 1.0f;
    float h = d[i+1];
    if (h != 0.0f) {
      for (int k = 0; k <= i; k++) {
        d[k] = V[k][i+1] / h;
      }
      for (int j = 0; j <= i; j++) {
        float g = 0.0f;
        for (int k = 0; k <= i; k++) {
          g += V[k][i+1] * V[k][j];
        }
        for (int k = 0; k <= i; k++) {
          V[k][j] -= g * d[k];
        }
      }
    }
    for (int k = 0; k <= i; k++) {
      V[k][i+1] = 0.0f;
    }
  }
  for (int j = 0; j < SIZE; j++) {
    d[j] = V[SIZE-1][j];
    V[SIZE-1][j] = 0.0f;
  }
  V[SIZE-1][SIZE-1] = 1.0f;
  e[0] = 0.0f;
} 

// Symmetric tridiagonal QL algorithm.

void tql2(float V[SIZE][SIZE], float d[SIZE], float e[SIZE]) {

//  This is derived from the Algol procedures tql2, by
//  Bowdler, Martin, Reinsch, and Wilkinson, Handbook for
//  Auto. Comp., Vol.ii-Linear Algebra, and the corresponding
//  Fortran subroutine in EISPACK.

  for (int i = 1; i < SIZE; i++) {
    e[i-1] = e[i];
  }
  e[SIZE-1] = 0.0f;

  float f = 0.0f;
  float tst1 = 0.0f;
  float eps = pow(2.0f,-52.0f);
  for (int l = 0; l < SIZE; l++) {

    // Find small subdiagonal element

    tst1 = MAX(tst1,fabs(d[l]) + fabs(e[l]));
    int m = l;
    while (m < SIZE) {
      if (fabs(e[m]) <= eps*tst1) {
        break;
      }
      m++;
    }

    // If m == l, d[l] is an eigenvalue,
    // otherwise, iterate.

    if (m > l) {
      int iter = 0;
      do {
        iter = iter + 1;  // (Could check iteration count here.)

        // Compute implicit shift

        float g = d[l];
        float p = (d[l+1] - g) / (2.0f * e[l]);
        float r = hypot2(p,1.0f);
        if (p < 0) {
          r = -r;
        }
        d[l] = e[l] / (p + r);
        d[l+1] = e[l] * (p + r);
        float dl1 = d[l+1];
        float h = g - d[l];
        for (int i = l+2; i < SIZE; i++) {
          d[i] -= h;
        }
        f = f + h;

        // Implicit QL transformation.

        p = d[m];
        float c = 1.0f;
        float c2 = c;
        float c3 = c;
        float el1 = e[l+1];
        float s = 0.0f;
        float s2 = 0.0f;
        for (int i = m-1; i >= l; i--) {
          c3 = c2;
          c2 = c;
          s2 = s;
          g = c * e[i];
          h = c * p;
          r = hypot2(p,e[i]);
          e[i+1] = s * r;
          s = e[i] / r;
          c = p / r;
          p = c * d[i] - s * g;
          d[i+1] = h + s * (c * g + s * d[i]);

          // Accumulate transformation.

          for (int k = 0; k < SIZE; k++) {
            h = V[k][i+1];
            V[k][i+1] = s * V[k][i] + c * h;
            V[k][i] = c * V[k][i] - s * h;
          }
        }
        p = -s * s2 * c3 * el1 * e[l] / dl1;
        e[l] = s * p;
        d[l] = c * p;

        // Check for convergence.

      } while (fabs(e[l]) > eps*tst1);
    }
    d[l] = d[l] + f;
    e[l] = 0.0f;
  }
  
  // Sort eigenvalues and corresponding vectors.

  for (int i = 0; i < SIZE-1; i++) {
    int k = i;
    float p = d[i];
    for (int j = i+1; j < SIZE; j++) {
      if (fabs(d[j]) < fabs(p)) {
        k = j;
        p = d[j];
      }
    }
    if (k != i) {
      d[k] = d[i];
      d[i] = p;
      for (int j = 0; j < SIZE; j++) {
        p = V[j][i];
        V[j][i] = V[j][k];
        V[j][k] = p;
      }
    }
  }
}

void eigen_decomposition(float A[SIZE][SIZE], float V[SIZE][SIZE], float d[SIZE]) {
  float e[SIZE];
  for (int i = 0; i < SIZE; i++) {
    for (int j = 0; j < SIZE; j++) {
      V[i][j] = A[i][j];
    }
  }
  tred2(V, d, e);
  tql2(V, d, e);
}

float dot(float3 a, float3 b) {
    return a.x*b.x+a.y*b.y+a.z*b.z;
}
int3 operator-(int3 a, int3 b) {
    int3 c;
    c.x = a.x-b.x;
    c.y = a.y-b.y;
    c.z = a.z-b.z;
    return c;
}

float3 normalize(float3 a) {
    float3 b;
    float sqrMag = sqrt(a.x*a.x+a.y*a.y+a.z*a.z);
    b.x = a.x / sqrMag;
    b.y = a.y / sqrMag;
    b.z = a.z / sqrMag;
    return b;
}
float3 normalize(int3 a) {
    float3 b;
    float sqrMag = sqrt(a.x*a.x+a.y*a.y+a.z*a.z);
    b.x = a.x / sqrMag;
    b.y = a.y / sqrMag;
    b.z = a.z / sqrMag;
    return b;
}

#define POS(pos) pos.x+pos.y*size.x+pos.z*size.x*size.y
float3 gradient(TubeSegmentation &TS, int3 pos, int volumeComponent, int dimensions, int3 size) {
    float * Fx = TS.Fx;
    float * Fy = TS.Fy;
    float * Fz = TS.Fz;
    float f100, f_100, f010, f0_10, f001, f00_1;
    int3 npos = pos;
    switch(volumeComponent) {
        case 0:

        npos.x +=1;
        f100 = Fx[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        npos.x -=2;
        f_100 = Fx[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        if(dimensions > 1) {
            npos = pos;
            npos.y += 1;
            f010 = Fx[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
            npos.y -= 2;
            f0_10 = Fx[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        }
        if(dimensions > 2) {
            npos = pos;
            npos.z += 1;
            f001 = Fx[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
            npos.z -= 2;
            f00_1 =Fx[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        }
    break;
        case 1:

        npos.x +=1;
        f100 = Fy[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        npos.x -=2;
        f_100 = Fy[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        if(dimensions > 1) {
            npos = pos;
            npos.y += 1;
            f010 = Fy[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
            npos.y -= 2;
            f0_10 = Fy[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        }
        if(dimensions > 2) {
            npos = pos;
            npos.z += 1;
            f001 = Fy[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
            npos.z -= 2;
            f00_1 =Fy[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        }
    break;
        case 2:

        npos.x +=1;
        f100 = Fz[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        npos.x -=2;
        f_100 = Fz[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        if(dimensions > 1) {
            npos = pos;
            npos.y += 1;
            f010 = Fz[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
            npos.y -= 2;
            f0_10 = Fz[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        }
        if(dimensions > 2) {
            npos = pos;
            npos.z += 1;
            f001 = Fz[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
            npos.z -= 2;
            f00_1 =Fz[POS(npos)]/sqrt(Fx[POS(npos)]*Fx[POS(npos)]+Fy[POS(npos)]*Fy[POS(npos)]+Fz[POS(npos)]*Fz[POS(npos)]);
        }
    break;
    }

    float3 grad = {
        0.5f*(f100-f_100), 
        0.5f*(f010-f0_10),
        0.5f*(f001-f00_1)
    };


    return grad;
}

float sign(float a) {
    return a < 0 ? -1.0f: 1.0f;
}

float3 getTubeDirection(TubeSegmentation &T, int3 pos, int3 size) {

    // Do gradient on Fx, Fy and Fz and normalization
    float3 Fx = gradient(T, pos,0,1,size);
    float3 Fy = gradient(T, pos,1,2,size);
    float3 Fz = gradient(T, pos,2,3,size);
    
    float Hessian[3][3] = {
        {Fx.x, Fy.x, Fz.x},
        {Fy.x, Fy.y, Fz.y},
        {Fz.x, Fz.y, Fz.z}
    };
    float eigenValues[3];
    float eigenVectors[3][3];
    eigen_decomposition(Hessian, eigenVectors, eigenValues);
    float3 e1 = {eigenVectors[0][0], eigenVectors[1][0], eigenVectors[2][0]};
    return e1;
}

void doEigen(TubeSegmentation &T, int3 pos, int3 size, float3 * lambda, float3 * e1, float3 * e2, float3 * e3) {

    // Do gradient on Fx, Fy and Fz and normalization
    float3 Fx = gradient(T, pos,0,1,size);
    float3 Fy = gradient(T, pos,1,2,size);
    float3 Fz = gradient(T, pos,2,3,size);
    
    float Hessian[3][3] = {
        {Fx.x, Fy.x, Fz.x},
        {Fy.x, Fy.y, Fz.y},
        {Fz.x, Fz.y, Fz.z}
    };
    float eigenValues[3];
    float eigenVectors[3][3];
    eigen_decomposition(Hessian, eigenVectors, eigenValues);
    e1->x = eigenVectors[0][0];
    e1->y = eigenVectors[1][0];
    e1->z = eigenVectors[2][0];
    e2->x = eigenVectors[0][1];
    e2->y = eigenVectors[1][1];
    e2->z = eigenVectors[2][1];
    e3->x = eigenVectors[0][2];
    e3->y = eigenVectors[1][2];
    e3->z = eigenVectors[2][2];
    lambda->x = eigenValues[0];
    lambda->y = eigenValues[1];
    lambda->z = eigenValues[2];
}


char * runRidgeTraversal(TubeSegmentation &T, SIPL::int3 size, paramList parameters, std::stack<CenterlinePoint> centerlineStack) {

    float Thigh = 0.6;
    int Dmin = 5;
    float Mlow = 0.2f;
    float Tlow = 0.4f;
    int maxBelowTlow = 2;
    float minMeanTube = 0.6f;
    int TreeMin = 10;
    const int totalSize = size.x*size.y*size.z;

    int * centerlines = new int[totalSize]();

    // Create queue
    std::priority_queue<point, std::vector<point>, PointComparison> queue;
    
    // Collect all valid start points
    #pragma omp parallel for 
    for(int z = 2; z < size.z-2; z++) {
        for(int y = 2; y < size.y-2; y++) {
            for(int x = 2; x < size.x-2; x++) {
                if(T.TDF[LPOS(x,y,z)] < Thigh)
                    continue;

                int3 pos = {x,y,z};
                bool valid = true;
                for(int a = -2; a < 2; a++) {
                    for(int b = -2; b < 2; b++) {
                        for(int c = -2; c < 2; c++) {
                            int3 nPos = {x+a,y+b,z+c};
                            if(T.TDF[POS(nPos)] > T.TDF[POS(pos)]) {
                                valid = false;
                                break;
                            }
                        }
                    }
                }

                if(valid) {
                    point p;
                    p.value = T.TDF[LPOS(x,y,z)];
                    p.x = x;
                    p.y = y;
                    p.z = z;
                    #pragma omp critical
                    queue.push(p);
                }
            }
        }
    }

    std::cout << "Processing " << queue.size() << " valid start points" << std::endl;
    int counter = 1;
    T.TDF[0] = 0;
    T.Fx[0] = 1;
    T.Fy[0] = 0;
    T.Fz[0] = 0;


    // Create a map of centerline distances
    std::unordered_map<int, int> centerlineDistances;

    // Create a map of centerline stacks
    std::unordered_map<int, std::stack<CenterlinePoint> > centerlineStacks;

    while(!queue.empty()) {
        // Traverse from new start point
        point p = queue.top();
        queue.pop();

        // Has it been handled before?
        if(centerlines[LPOS(p.x,p.y,p.z)] == 1)
            continue;

        char * newCenterlines = new char[totalSize]();
        newCenterlines[LPOS(p.x,p.y,p.z)] = 1;
        int distance = 1;
        int connections = 0;
        int prevConnection = -1;
        int secondConnection = -1;
        float meanTube = T.TDF[LPOS(p.x,p.y,p.z)];

        // Create new stack for this centerline
        std::stack<CenterlinePoint> stack;
        CenterlinePoint startPoint;
        startPoint.pos.x = p.x;
        startPoint.pos.y = p.y;
        startPoint.pos.z = p.z;

        stack.push(startPoint);

        // For each direction
        for(int direction = -1; direction < 3; direction += 2) {
            int belowTlow = 0;
            int3 position = {p.x,p.y,p.z};
            float3 t_i = getTubeDirection(T, position, size);
            t_i.x *= direction;
            t_i.y *= direction;
            t_i.z *= direction;
            float3 t_i_1;
            t_i_1.x = t_i.x;
            t_i_1.y = t_i.y;
            t_i_1.z = t_i.z;


            // Traverse
            while(true) {
                int3 maxPoint = {0,0,0};

                // Check for out of bounds
                if(position.x < 3 || position.x > size.x-3 || position.y < 3 || position.y > size.y-3 || position.z < 3 || position.z > size.z-3)
                    break;

                // Try to find next point from all neighbors
                for(int a = -1; a < 2; a++) {
                    for(int b = -1; b < 2; b++) {
                        for(int c = -1; c < 2; c++) {
                            int3 n = {position.x+a,position.y+b,position.z+c};
                            if((a == 0 && b == 0 && c == 0) || T.TDF[POS(n)] == 0.0f)
                                continue;

                            float3 dir = {(float)(n.x-position.x),(float)(n.y-position.y),(float)(n.z-position.z)};
                            dir = normalize(dir);
                            if( (dir.x*t_i.x+dir.y*t_i.y+dir.z*t_i.z) <= 0.1)
                                continue;

                            if(T.radius[POS(n)] >= 1.5f) {
                                if(M(n.x,n.y,n.z) > M(maxPoint.x,maxPoint.y,maxPoint.z)) 
                                maxPoint = n;
                            } else {
                                if(T.TDF[LPOS(n.x,n.y,n.z)]*M(n.x,n.y,n.z) > T.TDF[POS(maxPoint)]*M(maxPoint.x,maxPoint.y,maxPoint.z)) 
                                maxPoint = n;
                            }

                        }
                    }
                }

                if(maxPoint.x+maxPoint.y+maxPoint.z > 0) {
                    // New maxpoint found, check it!
                    if(centerlines[LPOS(maxPoint.x,maxPoint.y,maxPoint.z)] > 0) {
                        // Hit an existing centerline
                        if(prevConnection == -1) {
                            prevConnection = centerlines[LPOS(maxPoint.x,maxPoint.y,maxPoint.z)];
                        } else {
                            if(prevConnection ==centerlines[LPOS(maxPoint.x,maxPoint.y,maxPoint.z)]) {
                                // A loop has occured, reject this centerline
                                connections = 5;
                            } else {
                                secondConnection = centerlines[LPOS(maxPoint.x,maxPoint.y,maxPoint.z)];
                            }
                        }
                        break;
                    } else if(M(maxPoint.x,maxPoint.y,maxPoint.z) < Mlow || (belowTlow > maxBelowTlow && T.TDF[LPOS(maxPoint.x,maxPoint.y,maxPoint.z)] < Tlow)) {
                        // New point is below thresholds
                        break;
                    } else if(newCenterlines[LPOS(maxPoint.x,maxPoint.y,maxPoint.z)] == 1) {
                        // Loop detected!
                        break;
                    } else {
                        // Point is OK, proceed to add it and continue
                        if(T.TDF[LPOS(maxPoint.x,maxPoint.y,maxPoint.z)] < Tlow) {
                            belowTlow++;
                        } else {
                            belowTlow = 0;
                        }

                        // Update direction
                        //float3 e1 = getTubeDirection(T, maxPoint,size.x,size.y,size.z);

                        //TODO: check if all eigenvalues are negative, if so find the egeinvector that best matches
                        float3 lambda, e1, e2, e3;
                        doEigen(T, maxPoint, size, &lambda, &e1, &e2, &e3);
                        if((lambda.x < 0 && lambda.y < 0 && lambda.z < 0)) {
                            if(fabs(dot(t_i, e3)) > fabs(dot(t_i, e2))) {
                                if(fabs(dot(t_i, e3)) > fabs(dot(t_i, e1))) {
                                    e1 = e3;
                                }
                            } else if(fabs(dot(t_i, e2)) > fabs(dot(t_i, e1))) {
                                e1 = e2;
                            }
                        }


                        float maintain_dir = sign(dot(e1,t_i));
                        float3 vec_sum; 
                        vec_sum.x = maintain_dir*e1.x + t_i.x + t_i_1.x;
                        vec_sum.y = maintain_dir*e1.y + t_i.y + t_i_1.y;
                        vec_sum.z = maintain_dir*e1.z + t_i.z + t_i_1.z;
                        vec_sum = normalize(vec_sum);
                        t_i_1 = t_i;
                        t_i = vec_sum;

                        // update position
                        position = maxPoint;
                        distance ++;
                        newCenterlines[LPOS(maxPoint.x,maxPoint.y,maxPoint.z)] = 1;
                        meanTube += T.TDF[LPOS(maxPoint.x,maxPoint.y,maxPoint.z)];

                        // Create centerline point
                        CenterlinePoint p;
                        p.pos = position;
                        p.next = &(stack.top()); // add previous 
                        if(T.radius[POS(p.pos)] > 3.0f) {
                            p.large = true;
                        } else {
                            p.large = false;
                        }

                        // Add point to stack
                        stack.push(p);
                    }
                } else {
                    // No maxpoint found, stop!
                    break;
                }

            } // End traversal
        } // End for each direction

        // Check to see if new traversal can be added
        //std::cout << "Finished. Distance " << distance << " meanTube: " << meanTube/distance << std::endl;
        if(distance > Dmin && meanTube/distance > minMeanTube && connections < 2) {
            //std::cout << "Finished. Distance " << distance << " meanTube: " << meanTube/distance << std::endl;
            //std::cout << "------------------- New centerlines added #" << counter << " -------------------------" << std::endl;

            // TODO add it
            if(prevConnection == -1) {
                // No connections
		#pragma omp parallel for 
                for(int i = 0; i < totalSize;i++) {
                    if(newCenterlines[i] > 0)
                        centerlines[i] = counter;
                }
                centerlineDistances[counter] = distance;
                centerlineStacks[counter] = stack;
                counter ++;
            } else {
                // The first connection

                std::stack<CenterlinePoint> prevConnectionStack = centerlineStacks[prevConnection];
                while(!stack.empty()) {
                    prevConnectionStack.push(stack.top());
                    stack.pop();
                }

		#pragma omp parallel for
                for(int i = 0; i < totalSize;i++) {
                    if(newCenterlines[i] > 0)
                        centerlines[i] = prevConnection;
                }
                centerlineDistances[prevConnection] += distance;
                if(secondConnection != -1) {
                    // Two connections, move secondConnection to prevConnection
                    std::stack<CenterlinePoint> secondConnectionStack = centerlineStacks[secondConnection];
                    centerlineStacks.erase(secondConnection);
                    while(!secondConnectionStack.empty()) {
                        prevConnectionStack.push(secondConnectionStack.top());
                        secondConnectionStack.pop();
                    }

		    #pragma omp parallel for
                    for(int i = 0; i < totalSize;i++) {
                        if(centerlines[i] == secondConnection)
                            centerlines[i] = prevConnection;
                    }
                    centerlineDistances[prevConnection] += centerlineDistances[secondConnection];
                    centerlineDistances.erase(secondConnection);
                }

                centerlineStacks[prevConnection] = prevConnectionStack;
            }
        } // end if new point can be added
    } // End while queue is not empty

    if(centerlineDistances.size() == 0) {
        //throw SIPL::SIPLException("no centerlines were extracted");
        char * returnCenterlines = new char[totalSize]();
        return returnCenterlines;
    }

    // Find largest connected tree and all trees above a certain size
    std::unordered_map<int, int>::iterator it;
    int max = centerlineDistances.begin()->first;
    std::list<int> trees;
    for(it = centerlineDistances.begin(); it != centerlineDistances.end(); it++) {
        if(it->second > centerlineDistances[max])
            max = it->first;
        if(it->second > TreeMin) 
            trees.push_back(it->first);
    }
    std::list<int>::iterator it2;
    // TODO: if use the method with TreeMin have to add them to centerlineStack also
    centerlineStack = centerlineStacks[max];
    for(it2 = trees.begin(); it2 != trees.end(); it2++) {
        while(!centerlineStacks[*it2].empty()) {
            centerlineStack.push(centerlineStacks[*it2].top());
            centerlineStacks[*it2].pop();
        }
    }

    char * returnCenterlines = new char[totalSize]();
    // Mark largest tree with 1, and rest with 0
    #pragma omp parallel for
    for(int i = 0; i < totalSize;i++) {
        if(centerlines[i] == max) {
        //if(centerlines[i] > 0) {
            returnCenterlines[i] = 1;
        } else {
            bool valid = false;
            for(it2 = trees.begin(); it2 != trees.end(); it2++) {
                if(centerlines[i] == *it2) {
                    returnCenterlines[i] = 1;
                    valid = true;
                    break;
                }
            }
            if(!valid)
                returnCenterlines[i] = 0;
                    
        }
    }

	delete[] centerlines;
    return returnCenterlines;
}


float * createBlurMask(float sigma, int * maskSizePointer) {
    int maskSize = (int)ceil(3.0f*sigma);
    float * mask = new float[(maskSize*2+1)*(maskSize*2+1)*(maskSize*2+1)];
    float sum = 0.0f;
    for(int a = -maskSize; a < maskSize+1; a++) {
        for(int b = -maskSize; b < maskSize+1; b++) {
            for(int c = -maskSize; c < maskSize+1; c++) {
                sum += exp(-((float)(a*a+b*b+c*c) / (2*sigma*sigma)));
                mask[a+maskSize+(b+maskSize)*(maskSize*2+1)+(c+maskSize)*(maskSize*2+1)*(maskSize*2+1)] = exp(-((float)(a*a+b*b+c*c) / (2*sigma*sigma)));

            }
        }
    }
    for(int i = 0; i < (maskSize*2+1)*(maskSize*2+1)*(maskSize*2+1); i++)
        mask[i] = mask[i] / sum;

    *maskSizePointer = maskSize;

    return mask;
}

TubeSegmentation runCircleFittingMethod(OpenCL ocl, Image3D dataset, SIPL::int3 size, paramList parameters, Image3D &vectorField, Image3D &TDF, Image3D &radiusImage) {
    // Set up parameters
    const int GVFIterations = getParami(parameters, "gvf-iterations", 250);
    const float radiusMin = getParamf(parameters, "radius-min", 0.5);
    const float radiusMax = getParamf(parameters, "radius-min", 15.0);
    const float radiusStep = getParamf(parameters, "radius-step", 0.5);
    const float Fmax = getParamf(parameters, "fmax", 0.2);
    const int totalSize = size.x*size.y*size.z;
    const bool no3Dwrite = parameters.count("3d_write") == 0;
    const float MU = getParamf(parameters, "gvf-mu", 0.05);
    const int vectorSign = getParamstr(parameters, "mode", "black") == "black" ? -1 : 1;


    cl::size_t<3> offset;
    offset[0] = 0;
    offset[1] = 0;
    offset[2] = 0;
    cl::size_t<3> region;
    region[0] = size.x;
    region[1] = size.y;
    region[2] = size.z;

    // Create kernels
    Kernel blurVolumeWithGaussianKernel(ocl.program, "blurVolumeWithGaussian");
    Kernel createVectorFieldKernel(ocl.program, "createVectorField");
    Kernel circleFittingTDFKernel(ocl.program, "circleFittingTDF");
    Kernel combineKernel = Kernel(ocl.program, "combine");

    cl::Event startEvent, endEvent;
    cl_ulong start, end;
    INIT_TIMER

    int maskSize = 0;
    float * mask;// = createBlurMask(0.5, &maskSize);
    Buffer blurMask;


    /*
    // Run blurVolumeWithGaussian on processedVolume
    blurVolumeWithGaussianKernel.setArg(0, imageVolume);
    blurVolumeWithGaussianKernel.setArg(1, blurredVolume);
    blurVolumeWithGaussianKernel.setArg(2, maskSize);
    blurVolumeWithGaussianKernel.setArg(3, blurMask);
    queue.enqueueNDRangeKernel(
            blurVolumeWithGaussianKernel,
            NullRange,
            NDRange(size.x,size.y,size.z),
            NullRange
    );

    // Copy buffer to image
    queue.enqueueCopyBufferToImage(blurredVolume, imageVolume, 0, offset, region);
    */

#ifdef TIMING
    ocl.queue.enqueueMarker(&startEvent);
#endif
    if(no3Dwrite) {
        // Create auxillary buffer
        Buffer vectorFieldBuffer = Buffer(ocl.context, CL_MEM_WRITE_ONLY, 4*sizeof(float)*totalSize);
        vectorField = Image3D(ocl.context, CL_MEM_READ_ONLY, ImageFormat(CL_RGBA, CL_FLOAT), size.x, size.y, size.z);
 
        // Run create vector field
        createVectorFieldKernel.setArg(0, dataset);
        createVectorFieldKernel.setArg(1, vectorFieldBuffer);
        createVectorFieldKernel.setArg(2, Fmax);
        createVectorFieldKernel.setArg(3, vectorSign);

        ocl.queue.enqueueNDRangeKernel(
                createVectorFieldKernel,
                NullRange,
                NDRange(size.x,size.y,size.z),
                NullRange
        );

        // Copy buffer contents to image
        ocl.queue.enqueueCopyBufferToImage(
                vectorFieldBuffer, 
                vectorField, 
                0,
                offset,
                region
        );

    } else {
        vectorField = Image3D(ocl.context, CL_MEM_READ_WRITE, ImageFormat(CL_RGBA, CL_SNORM_INT16), size.x, size.y, size.z);
     
        // Run create vector field
        createVectorFieldKernel.setArg(0, dataset);
        createVectorFieldKernel.setArg(1, vectorField);
        createVectorFieldKernel.setArg(2, Fmax);
        createVectorFieldKernel.setArg(3, vectorSign);

        ocl.queue.enqueueNDRangeKernel(
                createVectorFieldKernel,
                NullRange,
                NDRange(size.x,size.y,size.z),
                NullRange
        );

    }
       
#ifdef TIMING
    ocl.queue.enqueueMarker(&endEvent);
    ocl.queue.finish();
    startEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &start);
    endEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &end);
    std::cout << "RUNTIME of Create vector field: " << (end-start)*1.0e-6 << " ms" << std::endl;
#endif
#ifdef TIMING
    ocl.queue.enqueueMarker(&startEvent);
#endif
    // Run circle fitting TDF kernel
    Buffer TDFsmall = Buffer(ocl.context, CL_MEM_WRITE_ONLY, sizeof(float)*totalSize);
    Buffer radiusSmall = Buffer(ocl.context, CL_MEM_WRITE_ONLY, sizeof(float)*totalSize);
    circleFittingTDFKernel.setArg(0, vectorField);
    circleFittingTDFKernel.setArg(1, TDFsmall);
    circleFittingTDFKernel.setArg(2, radiusSmall);
    circleFittingTDFKernel.setArg(3, radiusMin);
    circleFittingTDFKernel.setArg(4, 3.0f);
    circleFittingTDFKernel.setArg(5, 0.5f);

    ocl.queue.enqueueNDRangeKernel(
            circleFittingTDFKernel,
            NullRange,
            NDRange(size.x,size.y,size.z),
            NullRange
    );
    // Transfer buffer back to host
#ifdef TIMING
    ocl.queue.enqueueMarker(&endEvent);
    ocl.queue.finish();
    startEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &start);
    endEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &end);
    std::cout << "RUNTIME of TDF small: " << (end-start)*1.0e-6 << " ms" << std::endl;
#endif
    /* Large Airways */
    
#ifdef TIMING
    ocl.queue.enqueueMarker(&startEvent);
#endif
    Image3D blurredVolume = Image3D(ocl.context, CL_MEM_READ_WRITE, ImageFormat(CL_R, CL_FLOAT), size.x, size.y, size.z);

    mask = createBlurMask(1.0, &maskSize);
    blurMask = Buffer(ocl.context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(float)*(maskSize*2+1)*(maskSize*2+1)*(maskSize*2+1), mask);

    if(no3Dwrite) {
        // Create auxillary buffer
        Buffer blurredVolumeBuffer = Buffer(
                ocl.context, 
                CL_MEM_WRITE_ONLY, 
                sizeof(float)*totalSize
        );

        // Run blurVolumeWithGaussian on dataset
        blurVolumeWithGaussianKernel.setArg(0, dataset);
        blurVolumeWithGaussianKernel.setArg(1, blurredVolumeBuffer);
        blurVolumeWithGaussianKernel.setArg(2, maskSize);
        blurVolumeWithGaussianKernel.setArg(3, blurMask);

        ocl.queue.enqueueNDRangeKernel(
                blurVolumeWithGaussianKernel,
                NullRange,
                NDRange(size.x,size.y,size.z),
                NullRange
        );

        ocl.queue.enqueueCopyBufferToImage(
                blurredVolumeBuffer, 
                blurredVolume, 
                0,
                offset,
                region
        );
    } else {
        // Run blurVolumeWithGaussian on processedVolume
        blurVolumeWithGaussianKernel.setArg(0, dataset);
        blurVolumeWithGaussianKernel.setArg(1, blurredVolume);
        blurVolumeWithGaussianKernel.setArg(2, maskSize);
        blurVolumeWithGaussianKernel.setArg(3, blurMask);

        ocl.queue.enqueueNDRangeKernel(
                blurVolumeWithGaussianKernel,
                NullRange,
                NDRange(size.x,size.y,size.z),
                NullRange
        );
    }

#ifdef TIMING
    ocl.queue.enqueueMarker(&endEvent);
    ocl.queue.finish();
    startEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &start);
    endEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &end);
    std::cout << "RUNTIME blurring: " << (end-start)*1.0e-6 << " ms" << std::endl;
#endif
#ifdef TIMING
    ocl.queue.enqueueMarker(&startEvent);
#endif
   if(no3Dwrite) {
        // Create auxillary buffer
        Buffer vectorFieldBuffer = Buffer(ocl.context, CL_MEM_WRITE_ONLY, 4*sizeof(float)*totalSize);
        vectorField = Image3D(ocl.context, CL_MEM_READ_ONLY, ImageFormat(CL_RGBA, CL_FLOAT), size.x, size.y, size.z);
 
        // Run create vector field
        createVectorFieldKernel.setArg(0, blurredVolume);
        createVectorFieldKernel.setArg(1, vectorFieldBuffer);
        createVectorFieldKernel.setArg(2, Fmax);
        createVectorFieldKernel.setArg(3, vectorSign);

        ocl.queue.enqueueNDRangeKernel(
                createVectorFieldKernel,
                NullRange,
                NDRange(size.x,size.y,size.z),
                NullRange
        );

        // Copy buffer contents to image
        ocl.queue.enqueueCopyBufferToImage(
                vectorFieldBuffer, 
                vectorField, 
                0,
                offset,
                region
        );

    } else {
        vectorField = Image3D(ocl.context, CL_MEM_READ_WRITE, ImageFormat(CL_RGBA, CL_SNORM_INT16), size.x, size.y, size.z);
     
        // Run create vector field
        createVectorFieldKernel.setArg(0, blurredVolume);
        createVectorFieldKernel.setArg(1, vectorField);
        createVectorFieldKernel.setArg(2, Fmax);
        createVectorFieldKernel.setArg(3, vectorSign);

        ocl.queue.enqueueNDRangeKernel(
                createVectorFieldKernel,
                NullRange,
                NDRange(size.x,size.y,size.z),
                NullRange
        );

    } 
    
#ifdef TIMING
    ocl.queue.enqueueMarker(&endEvent);
    ocl.queue.finish();
    startEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &start);
    endEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &end);
    std::cout << "RUNTIME Create vector field: " << (end-start)*1.0e-6 << " ms" << std::endl;
#endif
#ifdef TIMING
    ocl.queue.enqueueMarker(&startEvent);
#endif

    // Run GVF on iVectorField as initial vector field
    Kernel GVFInitKernel = Kernel(ocl.program, "GVF3DInit");
    Kernel GVFIterationKernel = Kernel(ocl.program, "GVF3DIteration");
    Kernel GVFFinishKernel = Kernel(ocl.program, "GVF3DFinish");

    std::cout << "Running GVF with " << GVFIterations << " iterations " << std::endl; 
    if(no3Dwrite) {
        // Create auxillary buffers
        Buffer vectorFieldBuffer = Buffer(
                ocl.context,
                CL_MEM_READ_WRITE,
                3*sizeof(float)*totalSize
        );
        Buffer vectorFieldBuffer1 = Buffer(
                ocl.context,
                CL_MEM_READ_WRITE,
                3*sizeof(float)*totalSize
        );

        GVFInitKernel.setArg(0, vectorField);
        GVFInitKernel.setArg(1, vectorFieldBuffer);
        ocl.queue.enqueueNDRangeKernel(
                GVFInitKernel,
                NullRange,
                NDRange(size.x,size.y,size.z),
                NullRange
        );

        // Run iterations
        GVFIterationKernel.setArg(0, vectorField);
        GVFIterationKernel.setArg(3, MU);

        for(int i = 0; i < GVFIterations; i++) {
            if(i % 2 == 0) {
                GVFIterationKernel.setArg(1, vectorFieldBuffer);
                GVFIterationKernel.setArg(2, vectorFieldBuffer1);
            } else {
                GVFIterationKernel.setArg(1, vectorFieldBuffer1);
                GVFIterationKernel.setArg(2, vectorFieldBuffer);
            }
                ocl.queue.enqueueNDRangeKernel(
                        GVFIterationKernel,
                        NullRange,
                        NDRange(size.x,size.y,size.z),
                        NullRange
                );
        }

        vectorFieldBuffer1 = Buffer(
                ocl.context,
                CL_MEM_WRITE_ONLY,
                4*sizeof(float)*totalSize
        );

        // Copy vector field to image
        GVFFinishKernel.setArg(0, vectorFieldBuffer);
        GVFFinishKernel.setArg(1, vectorFieldBuffer1);

        ocl.queue.enqueueNDRangeKernel(
                GVFFinishKernel,
                NullRange,
                NDRange(size.x,size.y,size.z),
                NullRange
        );

        // Copy buffer contents to image
        ocl.queue.enqueueCopyBufferToImage(
                vectorFieldBuffer1, 
                vectorField, 
                0,
                offset,
                region
        );


    } else {
        Image3D vectorField1 = Image3D(ocl.context, CL_MEM_READ_WRITE, ImageFormat(CL_RGBA, CL_SNORM_INT16), size.x, size.y, size.z);
        Image3D initVectorField = Image3D(ocl.context, CL_MEM_READ_WRITE, ImageFormat(CL_RG, CL_SNORM_INT16), size.x, size.y, size.z);

        // init vectorField from image
        GVFInitKernel.setArg(0, vectorField);
        GVFInitKernel.setArg(1, vectorField1);
        GVFInitKernel.setArg(2, initVectorField);
        ocl.queue.enqueueNDRangeKernel(
                GVFInitKernel,
                NullRange,
                NDRange(size.x,size.y,size.z),
                NullRange
        );
        // Run iterations
        GVFIterationKernel.setArg(0, initVectorField);
        GVFIterationKernel.setArg(3, MU);

        for(int i = 0; i < GVFIterations; i++) {
            if(i % 2 == 0) {
                GVFIterationKernel.setArg(1, vectorField1);
                GVFIterationKernel.setArg(2, vectorField);
            } else {
                GVFIterationKernel.setArg(1, vectorField);
                GVFIterationKernel.setArg(2, vectorField1);
            }
                ocl.queue.enqueueNDRangeKernel(
                        GVFIterationKernel,
                        NullRange,
                        NDRange(size.x,size.y,size.z),
                        NullRange
                );
        }

        // Copy vector field to image
        GVFFinishKernel.setArg(0, vectorField1);
        GVFFinishKernel.setArg(1, vectorField);

        ocl.queue.enqueueNDRangeKernel(
                GVFFinishKernel,
                NullRange,
                NDRange(size.x,size.y,size.z),
                NullRange
        );
    }
#ifdef TIMING
    ocl.queue.enqueueMarker(&endEvent);
    ocl.queue.finish();
    startEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &start);
    endEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &end);
    std::cout << "RUNTIME of GVF: " << (end-start)*1.0e-6 << " ms" << std::endl;
#endif

#ifdef TIMING
    ocl.queue.enqueueMarker(&startEvent);
#endif
    // Run circle fitting TDF kernel on GVF result
    Buffer TDFlarge = Buffer(ocl.context, CL_MEM_WRITE_ONLY, sizeof(float)*totalSize);
    Buffer radiusLarge = Buffer(ocl.context, CL_MEM_WRITE_ONLY, sizeof(float)*totalSize);
    circleFittingTDFKernel.setArg(0, vectorField);
    circleFittingTDFKernel.setArg(1, TDFlarge);
    circleFittingTDFKernel.setArg(2, radiusLarge);
    circleFittingTDFKernel.setArg(3, 1.0f);
    circleFittingTDFKernel.setArg(4, radiusMax);
    circleFittingTDFKernel.setArg(5, 1.0f);

    ocl.queue.enqueueNDRangeKernel(
            circleFittingTDFKernel,
            NullRange,
            NDRange(size.x,size.y,size.z),
            NullRange
    );

#ifdef TIMING
    ocl.queue.enqueueMarker(&endEvent);
    ocl.queue.finish();
    startEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &start);
    endEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &end);
    std::cout << "RUNTIME of TDF large: " << (end-start)*1.0e-6 << " ms" << std::endl;
#endif
#ifdef TIMING
    ocl.queue.enqueueMarker(&startEvent);
#endif
    combineKernel.setArg(0, TDFsmall);
    combineKernel.setArg(1, radiusSmall);
    combineKernel.setArg(2, TDFlarge);
    combineKernel.setArg(3, radiusLarge);
 
    ocl.queue.enqueueNDRangeKernel(
            combineKernel,
            NullRange,
            NDRange(totalSize),
            NullRange
    );
    TDF = Image3D(ocl.context, CL_MEM_READ_WRITE, ImageFormat(CL_R, CL_FLOAT),
            size.x, size.y, size.z);
    ocl.queue.enqueueCopyBufferToImage(
        TDFlarge,
        TDF,
        0,
        offset,
        region
    );
    radiusImage = Image3D(ocl.context, CL_MEM_READ_WRITE, ImageFormat(CL_R, CL_FLOAT),
            size.x, size.y, size.z);
    ocl.queue.enqueueCopyBufferToImage(
        radiusLarge,
        radiusImage,
        0,
        offset,
        region
    );
#ifdef TIMING
    ocl.queue.enqueueMarker(&endEvent);
    ocl.queue.finish();
    startEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &start);
    endEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &end);
    std::cout << "RUNTIME of combine: " << (end-start)*1.0e-6 << " ms" << std::endl;
#endif

}

Image3D runInverseGradientSegmentation(OpenCL ocl, Image3D volume, Image3D vectorField, SIPL::int3 size, paramList parameters) {
    const int totalSize = size.x*size.y*size.z;
    const bool no3Dwrite = parameters.count("3d_write") == 0;
#ifdef TIMING
    cl::Event startEvent, endEvent;
    cl_ulong start, end;
    ocl.queue.enqueueMarker(&startEvent);
#endif

    Kernel dilateKernel = Kernel(ocl.program, "dilate");
    Kernel erodeKernel = Kernel(ocl.program, "erode");
    Kernel initGrowKernel = Kernel(ocl.program, "initGrowing");
    Kernel growKernel = Kernel(ocl.program, "grow");

    cl::size_t<3> offset;
    offset[0] = 0;
    offset[1] = 0;
    offset[2] = 0;
    cl::size_t<3> region;
    region[0] = size.x;
    region[1] = size.y;
    region[2] = size.z;



    int stopGrowing = 0;
    Buffer stop = Buffer(ocl.context, CL_MEM_WRITE_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(int), &stopGrowing);
    
    growKernel.setArg(1, vectorField);	
    growKernel.setArg(3, stop);

    int i = 0;
    int minimumIterations = 0;
    if(no3Dwrite) {
        Buffer volume2 = Buffer(
                ocl.context,
                CL_MEM_READ_WRITE,
                sizeof(char)*totalSize
        );
        ocl.queue.enqueueCopyImageToBuffer(
                volume, 
                volume2, 
                offset, 
                region, 
                0
        );
        initGrowKernel.setArg(0, volume);
        initGrowKernel.setArg(1, volume2);
        ocl.queue.enqueueNDRangeKernel(
            initGrowKernel,
            NullRange,
            NDRange(size.x, size.y, size.z),
            NullRange
        );
        ocl.queue.enqueueCopyBufferToImage(
                volume2,
                volume,
                0,
                offset,
                region
        );
        growKernel.setArg(0, volume);
        growKernel.setArg(2, volume2);
        while(stopGrowing == 0) {
            // run for at least 10 iterations
            if(i > minimumIterations) {
                stopGrowing = 1;
                ocl.queue.enqueueWriteBuffer(stop, CL_TRUE, 0, sizeof(int), &stopGrowing);
            }

            ocl.queue.enqueueNDRangeKernel(
                    growKernel,
                    NullRange,
                        NDRange(size.x, size.y, size.z),
                        NullRange
                    );
            if(i > minimumIterations)
                ocl.queue.enqueueReadBuffer(stop, CL_TRUE, 0, sizeof(int), &stopGrowing);
            i++;
            ocl.queue.enqueueCopyBufferToImage(
                    volume2,
                    volume,
                    0,
                    offset,
                    region
            );
        }

    } else {
        Image3D volume2 = Image3D(ocl.context, CL_MEM_READ_WRITE, ImageFormat(CL_R, CL_SIGNED_INT8), size.x, size.y, size.z);
        ocl.queue.enqueueCopyImage(volume, volume2, offset, offset, region);
        initGrowKernel.setArg(0, volume);
        initGrowKernel.setArg(1, volume2);
        ocl.queue.enqueueNDRangeKernel(
            initGrowKernel,
            NullRange,
            NDRange(size.x, size.y, size.z),
            NullRange
        );
        while(stopGrowing == 0) {
            // run for at least 10 iterations
            if(i > minimumIterations) {
                stopGrowing = 1;
                ocl.queue.enqueueWriteBuffer(stop, CL_TRUE, 0, sizeof(int), &stopGrowing);
            }
            if(i % 2 == 0) {
                growKernel.setArg(0, volume);
                growKernel.setArg(2, volume2);
            } else {
                growKernel.setArg(0, volume2);
                growKernel.setArg(2, volume);
            }

            ocl.queue.enqueueNDRangeKernel(
                    growKernel,
                    NullRange,
                        NDRange(size.x, size.y, size.z),
                        NullRange
                    );
            if(i > minimumIterations)
                ocl.queue.enqueueReadBuffer(stop, CL_TRUE, 0, sizeof(int), &stopGrowing);
            i++;
        }

    }

    std::cout << "segmentation result grown in " << i << " iterations" << std::endl;

    if(no3Dwrite) {
        Buffer volumeBuffer = Buffer(
                ocl.context,
                CL_MEM_WRITE_ONLY,
                sizeof(char)*totalSize
        );
        dilateKernel.setArg(0, volume);
        dilateKernel.setArg(1, volumeBuffer);
       
        ocl.queue.enqueueNDRangeKernel(
            dilateKernel,
            NullRange,
            NDRange(size.x, size.y, size.z),
            NullRange
        );

        ocl.queue.enqueueCopyBufferToImage(
                volumeBuffer,
                volume,
                0,
                offset,
                region);

        erodeKernel.setArg(0, volume);
        erodeKernel.setArg(1, volumeBuffer);
       
        ocl.queue.enqueueNDRangeKernel(
            erodeKernel,
            NullRange,
            NDRange(size.x, size.y, size.z),
            NullRange
        );
        ocl.queue.enqueueCopyBufferToImage(
            volumeBuffer,
            volume,
            0,
            offset,
            region
        );
    } else {
        Image3D volume2 = Image3D(ocl.context, CL_MEM_READ_WRITE, ImageFormat(CL_R, CL_SIGNED_INT8), size.x, size.y, size.z);
        dilateKernel.setArg(0, volume);
        dilateKernel.setArg(1, volume2);
       
        ocl.queue.enqueueNDRangeKernel(
            dilateKernel,
            NullRange,
            NDRange(size.x, size.y, size.z),
            NullRange
        );

        erodeKernel.setArg(0, volume2);
        erodeKernel.setArg(1, volume);
       
        ocl.queue.enqueueNDRangeKernel(
            erodeKernel,
            NullRange,
            NDRange(size.x, size.y, size.z),
            NullRange
        );
    }
#ifdef TIMING
    ocl.queue.enqueueMarker(&endEvent);
    ocl.queue.finish();
    startEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &start);
    endEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &end);
    std::cout << "RUNTIME of segmentation: " << (end-start)*1.0e-6 << " ms" << std::endl;
#endif

    return volume;
}

struct Vertex {
    int3 pos;
};

struct Edge {
    float cost;
};

typedef boost::adjacency_list<  // adjacency_list is a template depending on :
        boost::listS,               //  The container used for egdes : here, std::list.
        boost::vecS,                //  The container used for vertices: here, std::vector.
        boost::undirectedS,           //  directed or undirected edges ?.
        Vertex,                     //  The type that describes a Vertex.
        boost::property< boost::edge_weight_t, float >                         //  The type that describes an Edge
        > MyGraph;

typedef MyGraph::vertex_descriptor VertexID;
typedef MyGraph::edge_descriptor   EdgeID;

Image3D runNewCenterlineAlg(OpenCL ocl, SIPL::int3 size, paramList parameters, Image3D vectorField, Image3D TDF, Image3D radius) {

    // Find candidate centerpoints

    // Try to link the centerpoints

    // Find most connected components in graph
    const int totalSize = size.x*size.y*size.z;
    const bool no3Dwrite = parameters.count("3d_write") == 0;

    cl::size_t<3> offset;
    offset[0] = 0;
    offset[1] = 0;
    offset[2] = 0;
    cl::size_t<3> region;
    region[0] = size.x;
    region[1] = size.y;
    region[2] = size.z;

    TubeSegmentation T;
    T.TDF = new float[totalSize];
    T.radius = new float[totalSize];
    ocl.queue.enqueueReadImage(TDF, CL_TRUE, offset, region, 0, 0, T.TDF);
    ocl.queue.enqueueReadImage(radius, CL_TRUE, offset, region, 0, 0, T.radius);
    T.Fx = new float[totalSize];
    T.Fy = new float[totalSize];
    T.Fz = new float[totalSize];
    if(no3Dwrite) {
        float * Fs = new float[totalSize*4];
        ocl.queue.enqueueReadImage(vectorField, CL_TRUE, offset, region, 0, 0, Fs);
#pragma omp parallel for
        for(int i = 0; i < totalSize; i++) {
            T.Fx[i] = Fs[i*4];
            T.Fy[i] = Fs[i*4+1];
            T.Fz[i] = Fs[i*4+2];
        }
        delete[] Fs;
    } else {
        short * Fs = new short[totalSize*4];
        ocl.queue.enqueueReadImage(vectorField, CL_TRUE, offset, region, 0, 0, Fs);
#pragma omp parallel for
        for(int i = 0; i < totalSize; i++) {
            T.Fx[i] = MAX(-1.0f, Fs[i*4] / 32767.0f);
            T.Fy[i] = MAX(-1.0f, Fs[i*4+1] / 32767.0f);;
            T.Fz[i] = MAX(-1.0f, Fs[i*4+2] / 32767.0f);
        }
        delete[] Fs;
    }
 
    char * centerpoints = new char[totalSize]();

    MyGraph graph;
    boost::property_map<MyGraph, boost::edge_weight_t>::type weightmap = get(boost::edge_weight, graph);

    float thetaLimit = 0.5; 
    float TDFlimit = 0.5;
    // Find all points with T.value > TDFlimit
    std::multimap<float, int3> candidates;
    for(int z = 0; z < size.z; z++) {
    for(int y = 0; y < size.y; y++) {
    for(int x = 0; x < size.x; x++) {
        int3 n = {x,y,z};
        if(T.TDF[POS(n)] > TDFlimit) {
            candidates.insert( std::pair<float,int3>(-T.TDF[POS(n)], n) );
            //candidates.insert( std::pair<float,int3>(SQR_MAG(n), n) );
        }
    }}}  
    std::cout << "size of candidate stack: " << candidates.size() << std::endl;

    std::multimap<float,int3>::iterator it;
    for(it = candidates.begin(); it != candidates.end(); it++) {
        int3 x = it->second;
        float3 e1 = getTubeDirection(T, x, size); 
        bool invalid = false;
        float radius = T.radius[POS(x)];
        int maxD = round(MAX(radius, 3)); 
        std::stack<int3> remove;
        for(int a = -maxD; a <= maxD; a++) {
        for(int b = -maxD; b <= maxD; b++) {
        for(int c = -maxD; c <= maxD; c++) {
            int3 n = {x.x+a,x.y+b,x.z+c};
            if(!inBounds(n, size)) 
                continue;

            if(a == 0 && b == 0 && c == 0)
                continue;

            //if(abs(a) <= 1 && abs(b) <= 1 && abs(c) <= 1)
                //sum += TS.value[POS(n)];

            float3 r = {(float)(n.x-x.x),(float)(n.y-x.y),(float)(n.z-x.z)};
            float dp = dot(e1,r);
            float3 r_projected = {r.x-e1.x*dp,r.y-e1.y*dp,r.z-e1.z*dp};
            float theta = acos(dot(normalize(r), normalize(r_projected)));
            if(theta < thetaLimit && sqrt(r.x*r.x+r.y*r.y+r.z*r.z) < maxD) {
                if(SQR_MAG(n) < SQR_MAG(x)) {
                    invalid = true;
                    break;
                }    
            }
            if(centerpoints[POS(n)] == 1 && sqrt(r.x*r.x+r.y*r.y+r.z*r.z) < (float)maxD) { 
                if(SQR_MAG(n) > SQR_MAG(x) && T.radius[POS(x)] >= T.radius[POS(n)]) { // is x more in "center" than n
                    // x is better choice than n
                    remove.push(n);
                } else if(sqrt(r.x*r.x+r.y*r.y+r.z*r.z) < (float)maxD*0.5){
                    invalid = true;
                    break;
                }
            }    
        }}}  
        if(!invalid) {
            centerpoints[POS(x)] = 1;
            while(!remove.empty()) {
                int3 n = remove.top();
                remove.pop();
                centerpoints[POS(n)] = 0;
            }
            //VertexID v = boost::add_vertex(graph);
            //graph[v].pos = x;
        }
    }

    for(int z = 0; z < size.z; z++) {
    for(int y = 0; y < size.y; y++) {
    for(int x = 0; x < size.x; x++) {
        int3 n = {x,y,z};
        if(centerpoints[POS(n)] == 1) {
            VertexID v = boost::add_vertex(graph);
            graph[v].pos = n;
        }
    }}}

    std::cout << "nr of vertices: " << boost::num_vertices(graph) << std::endl;
    float maxDistance = 40.0f;
    delete[] centerpoints;
    char * centerpoints2 = new char[totalSize]();

    MyGraph::vertex_iterator vertexIt, vertexEnd;
    boost::tie(vertexIt, vertexEnd) = boost::vertices(graph);
    for(; vertexIt != vertexEnd; ++vertexIt) {
        int3 xa = graph[*vertexIt].pos;
        MyGraph::vertex_iterator vertexIt2, vertexEnd2;
        boost::tie(vertexIt2, vertexEnd2) = boost::vertices(graph);
        std::vector<VertexID> neighbors;
        for(; vertexIt2 != vertexEnd2; ++vertexIt2) {
            int3 xb = graph[*vertexIt2].pos;

            // First check distance
            int3 r = {xb.x - xa.x,xb.y-xa.y,xb.z-xa.z};
            if(sqrt(r.x*r.x+r.y*r.y+r.z*r.z) > maxDistance)
                continue;

           neighbors.push_back(*vertexIt2); 
        }

        if(neighbors.size() < 2)
            continue;
        // Go through each neighbor pair
        std::vector<VertexID>::iterator xb1it;
        std::vector<VertexID>::iterator xb2it;
        float shortestDistance = 9999.0f;
        VertexID bestPair1, bestPair2;
        bool validPairFound = false;
        //centerpoints2[POS((xa))] = 2;
        for(xb1it = neighbors.begin(); xb1it != neighbors.end(); xb1it++) {
            //centerpoints2[POS((graph[*xb1it].pos))] = 2;
        for(xb2it = neighbors.begin(); xb2it != neighbors.end(); xb2it++) {
            int3 * xb1 = &(graph[*xb1it].pos);
            int3 * xb2 = &(graph[*xb2it].pos);
            if(xb1->x == xb2->x && xb1->y == xb2->y && xb1->z == xb2->z)
                continue;
            if(xb1->x == xa.x && xb1->y == xa.y && xb1->z == xa.z)
                continue;
            if(xb2->x == xa.x && xb2->y == xa.y && xb2->z == xa.z)
                continue;
            //std::cout << "processing: " << xb1->x << ","<< xb1->y << "," << xb1->z << "  -  " << xb2->x << ","<< xb2->y << "," << xb2->z << std::endl;

            // Check angle
            int3 xb1v = {xb1->x-xa.x,xb1->y-xa.y,xb1->z-xa.z};
            int3 xb2v = {xb2->x-xa.x,xb2->y-xa.y,xb2->z-xa.z};
            float angle = acos(dot(normalize(xb1v), normalize(xb2v)));
            if(angle < 2.09f) // 120 degrees
                continue;

            // Check TDF
            float avgTDF = 0.0f;
            for(int i = 0; i < maxDistance; i++) {
                float alpha = (float)i/maxDistance;
                float3 n = {xa.x+xb1v.x*alpha,xa.y+xb1v.y*alpha,xa.z+xb1v.z*alpha};
                int3 r;
                r.x = round(n.x);
                r.y = round(n.y);
                r.z = round(n.z);
                avgTDF += T.TDF[POS(r)];
            }
            if(avgTDF / (maxDistance) < 0.5f)
                continue;
            avgTDF = 0.0f;
            for(int i = 0; i < maxDistance; i++) {
                float alpha = (float)i/maxDistance;
                float3 n = {xa.x+xb2v.x*alpha,xa.y+xb2v.y*alpha,xa.z+xb2v.z*alpha};
                int3 r;
                r.x = round(n.x);
                r.y = round(n.y);
                r.z = round(n.z);
                avgTDF += T.TDF[POS(r)];
            }
            if(avgTDF / (maxDistance) < 0.5f)
                continue;

            // If distance in shorter than previous, select it
            if(sqrt(xb1v.x*xb1v.x+xb1v.y*xb1v.y+xb1v.z*xb1v.z) + sqrt(xb2v.x*xb2v.x+xb2v.y*xb2v.y+xb2v.z*xb2v.z) < shortestDistance) {
                bestPair1 = *xb1it;
                bestPair2 = *xb2it;
                validPairFound = true;
                shortestDistance = sqrt(xb1v.x*xb1v.x+xb1v.y*xb1v.y+xb1v.z*xb1v.z) + sqrt(xb2v.x*xb2v.x+xb2v.y*xb2v.y+xb2v.z*xb2v.z);
            }
        }}

        // Add the selected pair to result
        if(validPairFound) {
            int3 xb1v = graph[bestPair1].pos-xa;
            int3 xb2v = graph[bestPair2].pos-xa;
            float avgGVF = 0.0f;
            for(int i = 0; i < maxDistance; i++) {
                float alpha = (float)i/maxDistance;
                float3 n = {xa.x+xb1v.x*alpha,xa.y+xb1v.y*alpha,xa.z+xb1v.z*alpha};
                int3 r;
                r.x = round(n.x);
                r.y = round(n.y);
                r.z = round(n.z);
                avgGVF += SQR_MAG(r);
            }
            float avgGVF2 = 0.0f;
            for(int i = 0; i < maxDistance; i++) {
                float alpha = (float)i/maxDistance;
                float3 n = {xa.x+xb2v.x*alpha,xa.y+xb2v.y*alpha,xa.z+xb2v.z*alpha};
                int3 r;
                r.x = round(n.x);
                r.y = round(n.y);
                r.z = round(n.z);
                avgGVF2 += SQR_MAG(r);
            }
            // add edge in graph
            EdgeID edge;
            EdgeID edge2;
            bool ok;
            boost::tie(edge, ok) = boost::add_edge(*vertexIt, bestPair1, graph);
            if(ok)
                weightmap[edge] = avgGVF / maxDistance;
            boost::tie(edge2, ok) = boost::add_edge(*vertexIt, bestPair2, graph);
            if(ok)
                weightmap[edge2] = avgGVF2 / maxDistance;
            /*
            centerpoints2[POS(xa)] = 2;
            centerpoints2[POS(bestPair1)] = 2;
            centerpoints2[POS(bestPair2)] = 2;
            */
        }

    }

    // Find most connected component in graph
    std::vector<int> c(num_vertices(graph));
    int num = boost::connected_components(graph, &c[0]);
    int * ccSize = new int[num]();
    for (int i = 0; i < c.size(); i++) {
        ccSize[c[i]]++;
    }
    int maxCC = 0;
    int minTreeLength = 20;
    bool * validTree = new bool[num];
    for(int i = 0; i < num; i++) {
        if(ccSize[i] > ccSize[maxCC])
            maxCC = i;
        validTree[i] = ccSize[i] > minTreeLength;
    }
    std::cout << "largest connected MST is of size: " << ccSize[maxCC] << std::endl;

    for(int i = 0; i < c.size(); i++) {
        if(c[i] == maxCC) {
        //if(validTree[c[i]]) {
            int3 xa = graph[vertex(i, graph)].pos;

            MyGraph::adjacency_iterator neighbourIt, neighbourEnd;
            boost::tie(neighbourIt, neighbourEnd) = adjacent_vertices(vertex(i,graph), graph);
            for(;neighbourIt != neighbourEnd; ++neighbourIt) {
                int3 xb = graph[*neighbourIt].pos;
                int3 r = xb - xa;
                for(int i = 0; i < maxDistance; i++) {
                    float alpha = (float)i/maxDistance;
                    float3 x = {xa.x+alpha*r.x,xa.y+alpha*r.y,xa.z+alpha*r.z};
                    int3 n;
                    n.x = round(x.x);
                    n.y = round(x.y);
                    n.z = round(x.z);
                    centerpoints2[POS(n)] = 1;
                }
                centerpoints2[POS(xb)] = 2;
                centerpoints2[POS(xa)] = 2;
            }

        } else {
            // Remove vertex from graph
            //remove_vertex(vertex(i, graph), graph);
        }
    }

    Image3D centerlineImage = Image3D(
            ocl.context,
            CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
            ImageFormat(CL_R, CL_SIGNED_INT8),
            size.x, size.y, size.z,
            0, 0, centerpoints2
    );

    return centerlineImage;
}

TubeSegmentation runCircleFittingAndNewCenterlineAlg(OpenCL ocl, cl::Image3D dataset, SIPL::int3 size, paramList parameters) {
    INIT_TIMER
    Image3D vectorField, TDF, radius;
    TubeSegmentation TS;
    const int totalSize = size.x*size.y*size.z;
    const bool no3Dwrite = parameters.count("3d_write") == 0;
    const std::string storageDirectory = getParamstr(parameters, "storage-dir", "/home/smistad/");

    cl::size_t<3> offset;
    offset[0] = 0;
    offset[1] = 0;
    offset[2] = 0;
    cl::size_t<3> region;
    region[0] = size.x;
    region[1] = size.y;
    region[2] = size.z;

    runCircleFittingMethod(ocl, dataset, size, parameters, vectorField, TDF, radius);
    Image3D centerline = runNewCenterlineAlg(ocl, size, parameters, vectorField, TDF, radius);
    TS.centerline = new char[totalSize];
    ocl.queue.enqueueReadImage(centerline, CL_TRUE, offset, region, 0, 0, TS.centerline);
    Image3D segmentation = runInverseGradientSegmentation(ocl, centerline, vectorField, size, parameters);


    // Transfer result back to host
    START_TIMER
    TS.segmentation = new char[totalSize];
    TS.TDF = new float[totalSize];
    TS.radius = new float[totalSize];
    ocl.queue.enqueueReadImage(TDF, CL_TRUE, offset, region, 0, 0, TS.TDF);
    ocl.queue.enqueueReadImage(radius, CL_TRUE, offset, region, 0, 0, TS.radius);
    ocl.queue.enqueueReadImage(segmentation, CL_TRUE, offset, region, 0, 0, TS.segmentation);
    TS.Fx = new float[totalSize];
    TS.Fy = new float[totalSize];
    TS.Fz = new float[totalSize];
    if(no3Dwrite) {
        float * Fs = new float[totalSize*4];
        ocl.queue.enqueueReadImage(vectorField, CL_TRUE, offset, region, 0, 0, Fs);
#pragma omp parallel for
        for(int i = 0; i < totalSize; i++) {
            TS.Fx[i] = Fs[i*4];
            TS.Fy[i] = Fs[i*4+1];
            TS.Fz[i] = Fs[i*4+2];
        }
        delete[] Fs;
    } else {
        short * Fs = new short[totalSize*4];
        ocl.queue.enqueueReadImage(vectorField, CL_TRUE, offset, region, 0, 0, Fs);
#pragma omp parallel for
        for(int i = 0; i < totalSize; i++) {
            TS.Fx[i] = MAX(-1.0f, Fs[i*4] / 32767.0f);
            TS.Fy[i] = MAX(-1.0f, Fs[i*4+1] / 32767.0f);;
            TS.Fz[i] = MAX(-1.0f, Fs[i*4+2] / 32767.0f);
        }
        delete[] Fs;
    }
    writeToRaw<char>(TS.centerline, storageDirectory + "centerline.raw", size.x, size.y, size.z);
    writeToRaw<char>(TS.segmentation, storageDirectory + "segmentation.raw", size.x, size.y, size.z);
    STOP_TIMER("writing segmentation and centerline to disk")

    return TS;
}

TubeSegmentation runCircleFittingAndRidgeTraversal(OpenCL ocl, Image3D dataset, SIPL::int3 size, paramList parameters) {
    
    INIT_TIMER
    cl::Event startEvent, endEvent;
    cl_ulong start, end;
    Image3D vectorField, TDF, radius;
    runCircleFittingMethod(ocl, dataset, size, parameters, vectorField, TDF, radius);
    const int totalSize = size.x*size.y*size.z;
    const bool no3Dwrite = parameters.count("3d_write") == 0;
    const std::string storageDirectory = getParamstr(parameters, "storage-dir", "/home/smistad/");

    cl::size_t<3> offset;
    offset[0] = 0;
    offset[1] = 0;
    offset[2] = 0;
    cl::size_t<3> region;
    region[0] = size.x;
    region[1] = size.y;
    region[2] = size.z;



    START_TIMER
    // Transfer buffer back to host
    TubeSegmentation TS;
    TS.Fx = new float[totalSize];
    TS.Fy = new float[totalSize];
    TS.Fz = new float[totalSize];
    if(no3Dwrite) {
        float * Fs = new float[totalSize*4];
        ocl.queue.enqueueReadImage(vectorField, CL_TRUE, offset, region, 0, 0, Fs);
#pragma omp parallel for
        for(int i = 0; i < totalSize; i++) {
            TS.Fx[i] = Fs[i*4];
            TS.Fy[i] = Fs[i*4+1];
            TS.Fz[i] = Fs[i*4+2];
        }
        delete[] Fs;
    } else {
        short * Fs = new short[totalSize*4];
        ocl.queue.enqueueReadImage(vectorField, CL_TRUE, offset, region, 0, 0, Fs);
#pragma omp parallel for
        for(int i = 0; i < totalSize; i++) {
            TS.Fx[i] = MAX(-1.0f, Fs[i*4] / 32767.0f);
            TS.Fy[i] = MAX(-1.0f, Fs[i*4+1] / 32767.0f);;
            TS.Fz[i] = MAX(-1.0f, Fs[i*4+2] / 32767.0f);
        }
        delete[] Fs;
    }
    TS.TDF = new float[totalSize];
    TS.radius = new float[totalSize];
    ocl.queue.enqueueReadImage(TDF, CL_TRUE, offset, region, 0, 0, TS.TDF);
    ocl.queue.enqueueReadImage(radius, CL_TRUE, offset, region, 0, 0, TS.radius);
    std::stack<CenterlinePoint> centerlineStack;
    TS.centerline = runRidgeTraversal(TS, size, parameters, centerlineStack);

    // Dilate the centerline
    Image3D volume = Image3D(ocl.context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, ImageFormat(CL_R, CL_SIGNED_INT8), size.x, size.y, size.z, 0, 0, TS.centerline);
#ifdef TIMING
    ocl.queue.finish();
    STOP_TIMER("Centerline extraction + transfer of data back and forth")
    ocl.queue.enqueueMarker(&startEvent);
#endif

    volume = runInverseGradientSegmentation(ocl, volume, vectorField, size, parameters);

    START_TIMER
    // Transfer volume back to host and write to disk
    TS.segmentation = new char[totalSize];
    ocl.queue.enqueueReadImage(volume, CL_TRUE, offset, region, 0, 0, TS.segmentation);
    writeToRaw<char>(TS.centerline, storageDirectory + "centerline.raw", size.x, size.y, size.z);
    writeToRaw<char>(TS.segmentation, storageDirectory + "segmentation.raw", size.x, size.y, size.z);
    STOP_TIMER("writing segmentation and centerline to disk")

    return TS;
}

paramList getParameters(int argc, char ** argv) {
    paramList parameters;
    // Go through each parameter, first parameter is filename
    for(int i = 2; i < argc; i++) {
        std::string token = argv[i];
        if(token.substr(0,2) == "--") {
            // Check to see if the parameter has a value
            std::string nextToken;
            if(i+1 < argc) {
                nextToken = argv[i+1];
            } else {
                nextToken = "--";
            }
            if(nextToken.substr(0,2) == "--") {
                // next token is not a value
                parameters[token.substr(2)] = "dummy-value";
            } else {
                // next token is a value, store the value
                parameters[token.substr(2)] = nextToken;
                i++;
            }
        }
    }

    return parameters;
}

Image3D readDatasetAndTransfer(OpenCL ocl, std::string filename, paramList parameters, SIPL::int3 * size) {
#ifdef TIMING
        cl_ulong start, end;
        Event startEvent, endEvent;
        ocl.queue.enqueueMarker(&startEvent);
#endif
    // Read mhd file, determine file type
    std::fstream mhdFile;
    mhdFile.open(filename.c_str(), std::fstream::in);
    if(!mhdFile)
        throw SIPL::FileNotFoundException(filename.c_str());
    std::string typeName = "";
    do {
        std::string line;
        std::getline(mhdFile, line);
        if(line.substr(0, 11) == "ElementType") 
            typeName = line.substr(11+3);
    } while(!mhdFile.eof());

    // Remove any trailing spaces
    int pos = typeName.find(" ");
    if(pos > 0)
        typeName = typeName.substr(0,pos);

    if(typeName == "") 
        throw SIPL::SIPLException("no data type defined in MHD file");

    // Read dataset using SIPL and transfer to device
    Image3D dataset;
    int type = 0;
    if(typeName == "MET_SHORT") {
        type = 1;
        SIPL::Volume<short> * v = new SIPL::Volume<short>(filename);
        dataset = Image3D(
                ocl.context, 
                CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                ImageFormat(CL_R, CL_SIGNED_INT16),
                v->getWidth(), v->getHeight(), v->getDepth(),
                0, 0, v->getData()
        );
        size->x = v->getWidth();
        size->y = v->getHeight();
        size->z = v->getDepth();
        delete v;
    } else if(typeName == "MET_USHORT") {
        type = 2;
        SIPL::Volume<SIPL::ushort> * v = new SIPL::Volume<SIPL::ushort>(filename);
        dataset = Image3D(
                ocl.context, 
                CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                ImageFormat(CL_R, CL_UNSIGNED_INT16),
                v->getWidth(), v->getHeight(), v->getDepth(),
                0, 0, v->getData()
        );
        size->x = v->getWidth();
        size->y = v->getHeight();
        size->z = v->getDepth();
        delete v;
    } else if(typeName == "MET_CHAR") {
        type = 1;
        SIPL::Volume<char> * v = new SIPL::Volume<char>(filename);
        dataset = Image3D(
                ocl.context, 
                CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                ImageFormat(CL_R, CL_SIGNED_INT8),
                v->getWidth(), v->getHeight(), v->getDepth(),
                0, 0, v->getData()
        );
        size->x = v->getWidth();
        size->y = v->getHeight();
        size->z = v->getDepth();
        delete v;
    } else if(typeName == "MET_UCHAR") {
        type = 2;
        SIPL::Volume<SIPL::uchar> * v = new SIPL::Volume<SIPL::uchar>(filename);
        dataset = Image3D(
                ocl.context, 
                CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                ImageFormat(CL_R, CL_UNSIGNED_INT8),
                v->getWidth(), v->getHeight(), v->getDepth(),
                0, 0, v->getData()
        );
        size->x = v->getWidth();
        size->y = v->getHeight();
        size->z = v->getDepth();
        delete v;
    } else if(typeName == "MET_FLOAT") {
        type = 3;
        SIPL::Volume<float> * v = new SIPL::Volume<float>(filename);
        dataset = Image3D(
                ocl.context, 
                CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                ImageFormat(CL_R, CL_FLOAT),
                v->getWidth(), v->getHeight(), v->getDepth(),
                0, 0, v->getData()
        );
        size->x = v->getWidth();
        size->y = v->getHeight();
        size->z = v->getDepth();
        delete v;
    } else {
        std::string msg = "unsupported filetype " + typeName;
        throw SIPL::SIPLException(msg.c_str());
    }
    std::cout << "Dataset of size " << size->x << " " << size->y << " " << size->z << " loaded" << std::endl;

    // Perform cropping if required
    if(parameters.count("cropping") == 1) {
        std::cout << "performing cropping" << std::endl;
#ifdef TIMING
        ocl.queue.enqueueMarker(&startEvent);
#endif
        Kernel cropDatasetKernel(ocl.program, "cropDataset");

        Buffer scanLinesInsideX = Buffer(ocl.context, CL_MEM_WRITE_ONLY, sizeof(short)*size->x);
        Buffer scanLinesInsideY = Buffer(ocl.context, CL_MEM_WRITE_ONLY, sizeof(short)*size->y);
        Buffer scanLinesInsideZ = Buffer(ocl.context, CL_MEM_WRITE_ONLY, sizeof(short)*size->z);
        cropDatasetKernel.setArg(0, dataset);
        cropDatasetKernel.setArg(1, scanLinesInsideX);
        cropDatasetKernel.setArg(2, 0);
        ocl.queue.enqueueNDRangeKernel(
            cropDatasetKernel,
            NullRange,
            NDRange(size->x),
            NullRange
        );
        cropDatasetKernel.setArg(1, scanLinesInsideY);
        cropDatasetKernel.setArg(2, 1);
        ocl.queue.enqueueNDRangeKernel(
            cropDatasetKernel,
            NullRange,
            NDRange(size->y),
            NullRange
        );
        cropDatasetKernel.setArg(1, scanLinesInsideZ);
        cropDatasetKernel.setArg(2, 2);
        ocl.queue.enqueueNDRangeKernel(
            cropDatasetKernel,
            NullRange,
            NDRange(size->z),
            NullRange
        );
        short * scanLinesX = new short[size->x];
        short * scanLinesY = new short[size->y];
        short * scanLinesZ = new short[size->z];
        ocl.queue.enqueueReadBuffer(scanLinesInsideX, CL_TRUE, 0, sizeof(short)*size->x, scanLinesX);
        ocl.queue.enqueueReadBuffer(scanLinesInsideY, CL_TRUE, 0, sizeof(short)*size->y, scanLinesY);
        ocl.queue.enqueueReadBuffer(scanLinesInsideZ, CL_TRUE, 0, sizeof(short)*size->z, scanLinesZ);

        int minScanLines = 200;
        int x1 = 0,x2 = size->x,y1 = 0,y2 = size->y,z1 = 0,z2 = size->z;
#pragma omp parallel sections
{
#pragma omp section
{
        for(int sliceNr = 0; sliceNr < size->x; sliceNr++) {
            if(scanLinesX[sliceNr] > minScanLines) {
                x1 = sliceNr;
                break;
            }
        }
}

#pragma omp section
{
        for(int sliceNr = size->x-1; sliceNr > 0; sliceNr--) {
            if(scanLinesX[sliceNr] > minScanLines) {
                x2 = sliceNr;
                break;
            }
        }
}
#pragma omp section
{
        for(int sliceNr = 0; sliceNr < size->y; sliceNr++) {
            if(scanLinesY[sliceNr] > minScanLines) {
                y1 = sliceNr;
                break;
            }
        }
}
#pragma omp section
{
        for(int sliceNr = size->y-1; sliceNr > 0; sliceNr--) {
            if(scanLinesY[sliceNr] > minScanLines) {
                y2 = sliceNr;
                break;
            }
        }
}
#pragma omp section
{
        for(int sliceNr = (size->z)/2; sliceNr < size->z; sliceNr++) {
            if(scanLinesZ[sliceNr] < minScanLines) {
                z2 = sliceNr;
                break;
            }
        }
}
#pragma omp section
{
        for(int sliceNr = (size->z)/2; sliceNr > 0; sliceNr--) {
            if(scanLinesZ[sliceNr] < minScanLines) {
                z1 = sliceNr;
                break;
            }
        }
}
}
        delete[] scanLinesX;
        delete[] scanLinesY;
        delete[] scanLinesZ;

        int SIZE_X = x2-x1;
        int SIZE_Y = y2-y1;
        int SIZE_Z = z2-z1;
	    // Make them dividable by 4
	    bool lower = false;
	    while(SIZE_X % 4 != 0) {
            if(lower && x1 > 0) {
                x1--;
            } else if(x2 < size->x) {
                x2++;
            }
            lower = !lower;
            SIZE_X = x2-x1;
	    }
	    while(SIZE_Y % 4 != 0) {
            if(lower && y1 > 0) {
                y1--;
            } else if(y2 < size->y) {
                y2++;
            }
            lower = !lower;
            SIZE_Y = y2-y1;
	    }
	    while(SIZE_Z % 4 != 0) {
            if(lower && z1 > 0) {
                z1--;
            } else if(z2 < size->z) {
                z2++;
            }
            lower = !lower;
            SIZE_Z = z2-z1;
	    }
        size->x = SIZE_X;
        size->y = SIZE_Y;
        size->z = SIZE_Z;
 

        std::cout << "Dataset cropped to " << SIZE_X << ", " << SIZE_Y << ", " << SIZE_Z << std::endl;
        Image3D imageHUvolume = Image3D(ocl.context, CL_MEM_READ_ONLY, ImageFormat(CL_R, CL_SIGNED_INT16), SIZE_X, SIZE_Y, SIZE_Z);

        cl::size_t<3> offset;
        offset[0] = 0;
        offset[1] = 0;
        offset[2] = 0;
        cl::size_t<3> region;
        region[0] = SIZE_X;
        region[1] = SIZE_Y;
        region[2] = SIZE_Z;
        cl::size_t<3> srcOffset;
        srcOffset[0] = x1;
        srcOffset[1] = y1;
        srcOffset[2] = z1;
        ocl.queue.enqueueCopyImage(dataset, imageHUvolume, srcOffset, offset, region);
        dataset = imageHUvolume;
#ifdef TIMING
        ocl.queue.enqueueMarker(&endEvent);
        ocl.queue.finish();
        startEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &start);
        endEvent.getProfilingInfo<cl_ulong>(CL_PROFILING_COMMAND_START, &end);
        std::cout << "Cropping time: " << (end-start)*1.0e-6 << " ms" << std::endl;
        ocl.queue.enqueueMarker(&startEvent);
#endif
    } // End cropping

    // Run toFloat kernel
    float minimum = 0.0f, maximum = 1.0f;
    if(parameters.count("minimum") == 1)
        minimum = atof(parameters["minimum"].c_str());
    
    if(parameters.count("maximum") == 1)
        maximum = atof(parameters["maximum"].c_str());

    Kernel toFloatKernel = Kernel(ocl.program, "toFloat");
    Image3D * convertedDataset = new Image3D(
        ocl.context,
        CL_MEM_READ_ONLY,
        ImageFormat(CL_R, CL_FLOAT),
        size->x, size->y, size->z
    );

    const bool no3Dwrite = parameters.count("3d_write") == 0;
    if(no3Dwrite) {
        Buffer convertedDatasetBuffer = Buffer(
                ocl.context, 
                CL_MEM_WRITE_ONLY,
                sizeof(float)*size->x*size->y*size->z
        );
        toFloatKernel.setArg(0, dataset);
        toFloatKernel.setArg(1, convertedDatasetBuffer);
        toFloatKernel.setArg(2, minimum);
        toFloatKernel.setArg(3, maximum);
        toFloatKernel.setArg(4, type);

        ocl.queue.enqueueNDRangeKernel(
            toFloatKernel,
            NullRange,
            NDRange(size->x, size->y, size->z),
            NullRange
        );

        cl::size_t<3> offset;
        offset[0] = 0;
        offset[1] = 0;
        offset[2] = 0;
        cl::size_t<3> region;
        region[0] = size->x;
        region[1] = size->y;
        region[2] = size->z;

        ocl.queue.enqueueCopyBufferToImage(
                convertedDatasetBuffer, 
                *convertedDataset, 
                0,
                offset,
                region
        );
    } else {
        toFloatKernel.setArg(0, dataset);
        toFloatKernel.setArg(1, *convertedDataset);
        toFloatKernel.setArg(2, minimum);
        toFloatKernel.setArg(3, maximum);
        toFloatKernel.setArg(4, type);

        ocl.queue.enqueueNDRangeKernel(
            toFloatKernel,
            NullRange,
            NDRange(size->x, size->y, size->z),
            NullRange
        );
    }

    // Return dataset
    return *convertedDataset;
}
