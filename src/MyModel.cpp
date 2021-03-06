#include "MyModel.h"
#include "DNest4.h"
#include "RNG.h"
#include "Utils.h"
#include "Data.h"
//#include "MultiSite2.h"
#include <cmath>
#include <fstream>
#include <chrono>
#include <typeinfo>  //for 'typeid' to work  

//#include "HODLR_Tree.hpp"
// #include <Eigen/Core>
// #include "celerite/celerite.h"


using namespace std;
using namespace Eigen;
using namespace DNest4;

// get the instance for the full dataset
//DataSet& full = DataSet::getRef("full");

#define DONEW false  
#define DOCEL false

#define trend false
#define GP true

// Uniform Cprior(-1000., 1000.);
ModifiedJeffreys Jprior(1.0, 99.); // additional white noise, m/s


MyModel::MyModel()
:objects(5, 1, true, MyConditionalPrior())
,mu(Data::get_instance().get_t().size())
,C(Data::get_instance().get_t().size(), Data::get_instance().get_t().size())
{}


void MyModel::from_prior(RNG& rng)
{
    objects.from_prior(rng);
    objects.consolidate_diff();
    
    double ymin, ymax, tmin, tmax;
    tmin = Data::get_instance().get_t_min();
    tmax = Data::get_instance().get_t_max();
    // ymin = Data::get_instance().get_y_min();
    // ymax = Data::get_instance().get_y_max();
    ymin = -1000.;
    ymax = 1000.;

    // background = Cprior.rvs(rng);
    background = ymin + (ymax - ymin)*rng.rand();

    extra_sigma = Jprior.rvs(rng);

    #if GP
        // eta1 = exp(log(1E-5) + log(1E-1)*rng.rand());
        eta1 = sqrt(3.); // m/s

        // eta2 = exp(log(1E-6) + log(1E6)*rng.rand());
        eta2 = 50.; //days

        // eta3 = 15. + 35.*rng.rand();
        eta3 = 20.; // days

        // exp(log(1E-5) + log(1E5)*rng.rand());
        eta4 = 0.5;
    #endif

    calculate_mu();

    #if GP
        calculate_C();
    #endif
}

void MyModel::calculate_C()
{

    // Get the data
    const vector<double>& t = Data::get_instance().get_t();
    const vector<double>& sig = Data::get_instance().get_sig();

    #if DONEW
        //auto begin = std::chrono::high_resolution_clock::now();  // start timing

        kernel->set_hyperpars(eta1, eta2, eta3, eta4);
        cout << eta1 << "   " << eta2 << "   " << eta3 << "   " << eta4 << "   ";
        VectorXd yvar(t.size());
        for (int i = 0; i < t.size(); ++i)
            yvar(i) = eta1*eta1 + sig[i] * sig[i] + extra_sigma * extra_sigma;

        //auto begin = std::chrono::high_resolution_clock::now();  // start timing
        A->assemble_Matrix(yvar, 1e-14, 's');
        //auto end = std::chrono::high_resolution_clock::now();
        //cout << "assembling up took " << std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count() << " ns" << std::endl;
        A->compute_Factor();

        //auto end = std::chrono::high_resolution_clock::now();

        //ofstream timerfile;
        //timerfile.open("timings.txt", std::ios_base::app);
        //timerfile.setf(ios::fixed,ios::floatfield);
        //timerfile << t.size() << '\t' << std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count() << " ns" << std::endl;

    #elif DOCEL
        // celerite!
        // auto begin1 = std::chrono::high_resolution_clock::now();  // start timing

        /*
        This implements the kernel in Eq (61) of Foreman-Mackey et al. (2017)
        The kernel has parameters a, b, c and P
        corresponding to an amplitude, factor, decay timescale and period.
        */

        VectorXd alpha_real(1),
                 beta_real(1),
                 alpha_complex_real(1),
                 alpha_complex_imag(1),
                 beta_complex_real(1),
                 beta_complex_imag(1);
        
        a = eta1;
        b = eta4;
        P = eta3;
        c = eta2;

        alpha_real << a*(1.+b)/(2.+b);
        beta_real << c;
        alpha_complex_real << a/(2.+b);
        alpha_complex_imag << 0.;
        beta_complex_real << c;
        beta_complex_imag << 2.*M_PI / P;


        VectorXd yvar(t.size()), tt(t.size());
        for (int i = 0; i < t.size(); ++i){
            yvar(i) = sig[i] * sig[i] + extra_sigma * extra_sigma;
            tt(i) = t[i];
        }

        solver.compute(
            alpha_real, beta_real,
            alpha_complex_real, alpha_complex_imag,
            beta_complex_real, beta_complex_imag,
            tt, yvar  // Note: this is the measurement _variance_
        );


        // auto end1 = std::chrono::high_resolution_clock::now();
        // cout << "new GP: " << std::chrono::duration_cast<std::chrono::nanoseconds>(end1-begin1).count() << " ns" << std::endl;
        
    #else

        int N = Data::get_instance().get_t().size();
        // auto begin = std::chrono::high_resolution_clock::now();  // start timing

        for(size_t i=0; i<N; i++)
        {
            for(size_t j=i; j<N; j++)
            {
                //C(i, j) = eta1*eta1*exp(-0.5*pow((t[i] - t[j])/eta2, 2) );
                C(i, j) = eta1*eta1*exp(-0.5*pow((t[i] - t[j])/eta2, 2) 
                           -2.0*pow(sin(M_PI*(t[i] - t[j])/eta3)/eta4, 2) );

                if(i==j)
                    C(i, j) += sig[i]*sig[i] + extra_sigma*extra_sigma; //+ eta5*t[i]*t[i];
                else
                    C(j, i) = C(i, j);
            }
        }

        // auto end = std::chrono::high_resolution_clock::now();
        // cout << "old GP: " << std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count() << " ns" << "\t"; // << std::endl;

    #endif
}

void MyModel::calculate_mu()
{
    // Get the times from the data
    const vector<double>& t = Data::get_instance().get_t();

    // Update or from scratch?
    bool update = (objects.get_added().size() < objects.get_components().size()) &&
            (staleness <= 10);

    // Get the components
    const vector< vector<double> >& components = (update)?(objects.get_added()):
                (objects.get_components());
    // at this point, components has:
    //  if updating: only the added planets' parameters
    //  if from scratch: all the planets' parameters

    // Zero the signal
    if(!update) // not updating, means recalculate everything
    {
        mu.assign(mu.size(), background);
        staleness = 0;
        #if trend
            for(size_t i=0; i<t.size(); i++)
                mu[i] += slope*(t[i] - t[0]) + quad*(t[i] - t[0])*(t[i] - t[0]);
            
            // cout << slope << "\t" << quad << endl;
        #endif
    }
    else // just updating (adding) planets
        staleness++;

    //auto begin = std::chrono::high_resolution_clock::now();  // start timing

    double P, K, phi, ecc, viewing_angle, f, v, ti;
    for(size_t j=0; j<components.size(); j++)
    {
        // P = exp(components[j][0]);
        P = components[j][0];
        K = components[j][1];
        phi = components[j][2];
        ecc = components[j][3];
        viewing_angle = components[j][4];

        for(size_t i=0; i<t.size(); i++)
        {
            ti = t[i];
            f = true_anomaly(ti, P, ecc, t[0]-(P*phi)/(2.*M_PI));
            v = K*(cos(f+viewing_angle) + ecc*cos(viewing_angle));
            mu[i] += v;
        }
    }

    //cout<<something<<endl;
}

double MyModel::perturb(RNG& rng)
{
    double logH = 0.;

    if(rng.rand() <= 0.5)
    {
        logH += objects.perturb(rng);
        objects.consolidate_diff();
        calculate_mu();
    }

    #if GP
        // else if(rng.rand() <= 0.5)
        // {
        //     if(rng.rand() <= 0.25)
        //     {
        //         eta1 = log(eta1);
        //         eta1 += log(1E4)*rng.randh(); // range of prior support
        //         wrap(eta1, log(1E-5), log(1E-1)); // wrap around inside prior
        //         eta1 = exp(eta1);
        //     }
        //     else if(rng.rand() <= 0.33330)
        //     {
        //         eta2 = log(eta2);
        //         eta2 += log(1E12)*rng.randh(); // range of prior support
        //         wrap(eta2, log(1E-6), log(1E6)); // wrap around inside prior
        //         eta2 = exp(eta2);
        //     }
        //     else if(rng.rand() <= 0.5)
        //     {
        //         eta3 += 35.*rng.randh(); // range of prior support
        //         wrap(eta3, 15., 50.); // wrap around inside prior
        //     }
        //     else
        //     {
        //         // eta4 = 1.0;

        //         eta4 = log(eta4);
        //         eta4 += log(1E10)*rng.randh(); // range of prior support
        //         wrap(eta4, log(1E-5), log(1E5)); // wrap around inside prior
        //         eta4 = exp(eta4);

        //         // eta4 += rng.randh();
        //         // wrap(eta4, 0., 1.);
        //     }

        //     calculate_C();

        // }
    #endif // GP

    else if(rng.rand() <= 0.5)
    {
        extra_sigma = Jprior.rvs(rng);

        #if GP
            calculate_C();
        #endif
    }
    else
    {

        for(size_t i=0; i<mu.size(); i++)
            mu[i] -= background;

        double ymin, ymax;
        ymin = -1000.;
        ymax = 1000.;
        // ymin = Data::get_instance().get_y_min();
        // ymax = Data::get_instance().get_y_max();

        background += (ymax - ymin)*rng.randh();
        wrap(background, ymin, ymax);

        for(size_t i=0; i<mu.size(); i++)
            mu[i] += background;

    }

    return logH;
}


double MyModel::log_likelihood() const
{
    int N = Data::get_instance().get_y().size();

    /** The following code calculates the log likelihood in the case of a GP model */

    // Get the data
    const vector<double>& y = Data::get_instance().get_y();

    //auto begin = std::chrono::high_resolution_clock::now();  // start timing
    #if GP

    #if DONEW
        // Set up the kernel.
        //auto begin = std::chrono::high_resolution_clock::now();  // start timing
        //QPkernel kernel;
        //kernel->set_hyperpars(eta1, eta2, eta3, eta4);
        //auto end = std::chrono::high_resolution_clock::now();
        //cout << "set kernel took " << std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count() << " ns" << std::endl;

        // Setting things up
        //auto begin = std::chrono::high_resolution_clock::now();  // start timing
        
        //auto end = std::chrono::high_resolution_clock::now();
        //cout << "setting up took " << std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count() << " ns" << std::endl;
        
        MatrixXd b(y.size(), 1), x;
        //VectorXd yvar(t.size());
        for (int i = 0; i < y.size(); ++i) {
            //yvar(i) = eta1*eta1 + sig[i] * sig[i] + extra_sigma * extra_sigma;
            b(i, 0) = y[i] - mu[i];
        }

        //auto begin = std::chrono::high_resolution_clock::now();  // start timing
        A->solve(b, x);
        double determinant;
        A->compute_Determinant(determinant);
        //auto end = std::chrono::high_resolution_clock::now();
        //cout << "solve and determinant took " << std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count() << " ns" << std::endl;

        //cout << logDeterminant << "   " << determinant << endl;
        //assert (logDeterminant == determinant);
        double exponent2 = 0.;
        for(int i = 0; i < y.size(); ++i)
            exponent2 += b(i,0)*x(i);


        double logL = -0.5*y.size()*log(2*M_PI)
                        - 0.5*determinant - 0.5*exponent2;

        //cout << logL << endl;
        //cout << logL << "   " << logL2 << endl;    
        //assert (logL == logL2);


    #else
        // residual vector (observed y minus model y)
        VectorXd residual(y.size());
        for(size_t i=0; i<y.size(); i++)
            residual(i) = y[i] - mu[i];

        #if DOCEL
            // logDeterminant = solver.log_determinant();
            // VectorXd solution = solver.solve(residual);

            double logL = -0.5 * (solver.dot_solve(residual) +
                                  solver.log_determinant() +
                                  y.size()*log(2*M_PI)); 
        #else
            // perform the cholesky decomposition of C
            Eigen::LLT<Eigen::MatrixXd> cholesky = C.llt();
            // get the lower triangular matrix L
            MatrixXd L = cholesky.matrixL();

            double logDeterminant = 0.;
            for(size_t i=0; i<y.size(); i++)
                logDeterminant += 2.*log(L(i,i));

            VectorXd solution = cholesky.solve(residual);

            // y*solution
            double exponent = 0.;
            for(size_t i=0; i<y.size(); i++)
                exponent += residual(i)*solution(i);

            double logL = -0.5*y.size()*log(2*M_PI)
                            - 0.5*logDeterminant - 0.5*exponent;
        #endif

        // calculate C^-1*(y-mu)
        // auto begin = std::chrono::high_resolution_clock::now();  // start timing
        // auto end = std::chrono::high_resolution_clock::now();
        // cout << "solve took " << std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count() << " ns \t";// <<  std::endl;

        // auto begin1 = std::chrono::high_resolution_clock::now();  // start timing
        // auto end1 = std::chrono::high_resolution_clock::now();
        // cout << "solve took " << std::chrono::duration_cast<std::chrono::nanoseconds>(end1-begin1).count() << " ns" << std::endl;
    #endif


    //auto end = std::chrono::high_resolution_clock::now();
    ////cout << "Likelihood took " << std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count() << " ns" << std::endl;

    #else

    /** The following code calculates the log likelihood in the case of a t-Student model without correlated noise*/
    //  for(size_t i=0; i<y.size(); i++)
    //  {
    //      var = sig[i]*sig[i] + extra_sigma*extra_sigma;
    //      logL += gsl_sf_lngamma(0.5*(nu + 1.)) - gsl_sf_lngamma(0.5*nu)
    //          - 0.5*log(M_PI*nu) - 0.5*log(var)
    //          - 0.5*(nu + 1.)*log(1. + pow(y[i] - mu[i], 2)/var/nu);
    //  }

    /** The following code calculates the log likelihood in the case of a Gaussian likelihood*/
    const vector<double>& sig = Data::get_instance().get_sig();

    double halflog2pi = 0.5*log(2.*M_PI);
    double logL = 0.;
    double var;
    for(size_t i=0; i<y.size(); i++)
    {
        var = sig[i]*sig[i] + extra_sigma*extra_sigma;
        logL += - halflog2pi - 0.5*log(var)
                - 0.5*(pow(y[i] - mu[i], 2)/var);
    }

    #endif // GP


    if(std::isnan(logL) || std::isinf(logL))
        logL = -1E300;
    return logL;
}

void MyModel::print(std::ostream& out) const
{
    // output presision
    out.setf(ios::fixed,ios::floatfield);
    out.precision(8);

    out<<extra_sigma<<'\t';

    #if GP
        out<<eta1<<'\t'<<eta2<<'\t'<<eta3<<'\t'<<eta4<<'\t';
    #endif
  
    objects.print(out); out<<' '<<staleness<<' ';
    out<<background<<' ';
}

string MyModel::description() const
{
    #if #GP
        return string("extra_sigma   eta1   eta2   eta3   eta4  objects.print   staleness   background");
    #else
        return string("extra_sigma   objects.print   staleness   background");
    #endif
}


/**
    Calculates the eccentric anomaly at time t by solving Kepler's equation.
    See "A Practical Method for Solving the Kepler Equation", Marc A. Murison, 2006

    @param t the time at which to calculate the eccentric anomaly.
    @param period the orbital period of the planet
    @param ecc the eccentricity of the orbit
    @param t_peri time of periastron passage
    @return eccentric anomaly.
*/
double MyModel::ecc_anomaly(double t, double period, double ecc, double time_peri)
{
    double tol;
    if (ecc < 0.8) tol = 1e-14;
    else tol = 1e-13;

    double n = 2.*M_PI/period;  // mean motion
    double M = n*(t - time_peri);  // mean anomaly
    double Mnorm = fmod(M, 2.*M_PI);
    double E0 = keplerstart3(ecc, Mnorm);
    double dE = tol + 1;
    double E;
    int count = 0;
    while (dE > tol)
    {
        E = E0 - eps3(ecc, Mnorm, E0);
        dE = abs(E-E0);
        E0 = E;
        count++;
        // failed to converge, this only happens for nearly parabolic orbits
        if (count == 100) break;
    }
    return E;
}


/**
    Provides a starting value to solve Kepler's equation.
    See "A Practical Method for Solving the Kepler Equation", Marc A. Murison, 2006

    @param e the eccentricity of the orbit
    @param M mean anomaly (in radians)
    @return starting value for the eccentric anomaly.
*/
double MyModel::keplerstart3(double e, double M)
{
    double t34 = e*e;
    double t35 = e*t34;
    double t33 = cos(M);
    return M + (-0.5*t35 + e + (t34 + 1.5*t33*t35)*t33)*sin(M);
}


/**
    An iteration (correction) method to solve Kepler's equation.
    See "A Practical Method for Solving the Kepler Equation", Marc A. Murison, 2006

    @param e the eccentricity of the orbit
    @param M mean anomaly (in radians)
    @param x starting value for the eccentric anomaly
    @return corrected value for the eccentric anomaly
*/
double MyModel::eps3(double e, double M, double x)
{
    double t1 = cos(x);
    double t2 = -1 + e*t1;
    double t3 = sin(x);
    double t4 = e*t3;
    double t5 = -x + t4 + M;
    double t6 = t5/(0.5*t5*t4/t2+t2);

    return t5/((0.5*t3 - 1/6*t1*t6)*e*t6+t2);
}



/**
    Calculates the true anomaly at time t.
    See Eq. 2.6 of The Exoplanet Handbook, Perryman 2010

    @param t the time at which to calculate the true anomaly.
    @param period the orbital period of the planet
    @param ecc the eccentricity of the orbit
    @param t_peri time of periastron passage
    @return true anomaly.
*/
double MyModel::true_anomaly(double t, double period, double ecc, double t_peri)
{
    double E = ecc_anomaly(t, period, ecc, t_peri);
    double f = acos( (cos(E)-ecc)/( 1-ecc*cos(E) ) );
    //acos gives the principal values ie [0:PI]
    //when E goes above PI we need another condition
    if(E>M_PI)
      f=2*M_PI-f;

    return f;
}
