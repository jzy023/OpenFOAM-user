/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | www.openfoam.com
     \\/     M anipulation  |
-------------------------------------------------------------------------------
    Copyright (C) 2015 OpenFOAM Foundation
    Copyright (C) 2016 OpenCFD Ltd.
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "myEddy.H"
#include "mathematicalConstants.H"

using namespace Foam::constant;

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

Foam::label Foam::myEddy::Gamma2Values[] = {1, 2, 3, 4, 5, 6, 7, 8};
Foam::UList<Foam::label> Foam::myEddy::Gamma2(&Gamma2Values[0], 8);
int Foam::myEddy::debug = 0;

// * * * * * * * * * * * * * Private Member Functions  * * * * * * * * * * * //

bool Foam::myEddy::setScales
(
    const scalar sigmaX,
    const label gamma2,
    const vector& e,
    const vector& lambda,
    vector& sigma,
    vector& alpha,
    const scalar crr
) const
{
    // Static array of gamma^2 vs c2 coefficient
    static const scalar gamma2VsC2[8] =
        {2, 1.875, 1.737, 1.75, 0.91, 0.825, 0.806, 1.5};

    scalar gamma = Foam::sqrt(scalar(gamma2));

    // c2 coefficient retrieved from array
    scalar c2 = gamma2VsC2[gamma2 - 1];

    // Length scale in largest eigenvalue direction
    label d1 = dir1_;
    label d2 = (d1 + 1) % 3;
    label d3 = (d1 + 2) % 3;

    sigma[d1] = sigmaX;

    // Note: sigma_average = 1/3*(sigma_x + sigma_y + sigma_z)
    // Substituting for sigma_y = sigma_x/gamma and sigma_z = sigma_y
    //sigma[d1] = 3*sigmaX/(1 + 2/gamma);
    // Other length scales equal, as function of major axis length and gamma
    sigma[d2] = sigma[d1]/gamma;
    sigma[d3] = sigma[d2];

    vector sigma2 = cmptMultiply(sigma, sigma);
    scalar slos2 = cmptSum(cmptDivide(lambda, sigma2));

    bool ok = true;

    for (label beta = 0; beta < 3; ++beta)
    {
        scalar x = slos2 - 2*lambda[beta]/sigma2[beta];

        if (x < 0)
        {
            alpha[beta] = 0;
            ok = false;
        }
        else
        {
            // alpha[beta] = e[beta]*sqrt(x/(2*c2));
            // Edited by Yan 18.04.2020 for ABL with delta(H) = 1e3 m
            // k0 = 7.5e-4
            alpha[beta] = e[beta]*sqrt(crr*x/(2*c2));
            // if (beta == dir1_){
            //     alpha[beta] = crr*e[beta]*sqrt(x/(2*c2));
            // }
            // else{
            //     alpha[beta] = (crr/gamma)*e[beta]*sqrt(x/(2*c2));
            // }
        }
    }

    if (debug > 1)
    {
        Pout<< "c2:" << c2
            << ", gamma2:" << gamma2
            << ", gamma:" << gamma
            << ", lambda:" << lambda
            << ", sigma2: " << sigma2
            << ", slos2: " << slos2
            << ", sigmaX:" << sigmaX
            << ", sigma:" << sigma
            << ", alpha:" << alpha
            << endl;
    }

    return ok;
}


// * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * * * //

Foam::myEddy::myEddy()
:
    patchFaceI_(-1),
    position0_(Zero),
    x_(0),
    sigma_(Zero),
    alpha_(Zero),
    Rpg_(tensor::I),
    c1_(-1),
    dir1_(0)
{}


Foam::myEddy::myEddy
(
    const label patchFaceI,
    const point& position0,
    const scalar x,
    const scalar sigmaX,
    const symmTensor& R,
    Random& rndGen,
    const scalar crr
)
:
    patchFaceI_(patchFaceI),
    position0_(position0),
    x_(x),
    sigma_(Zero),
    alpha_(Zero),
    Rpg_(tensor::I),
    c1_(-1),
    dir1_(0)
{
    // Principal stresses - eigenvalues returned in ascending order
    vector lambda = eigenValues(R);

    // Eddy rotation from principal-to-global axes
    // - given by the 3 eigenvectors of the Reynold stress tensor as rows in
    //   the result tensor (transposed transformation tensor)
    // - returned in ascending eigenvalue order
    Rpg_ = eigenVectors(R, lambda).T();

    if (debug)
    {
        // Global->Principal transform = Rgp = Rpg.T()
        // Rgp & R & Rgp.T() should have eigenvalues on its diagonal and
        // zeros for all other components
        Pout<< "Rpg.T() & R & Rpg: " << (Rpg_.T() & R & Rpg_) << endl;
    }

    // Set the eddy orientation to position of max eigenvalue
    // (direction of eddy major axis, sigma_x in reference)
    dir1_ = 2;

    // Random vector of 1's and -1's
    const vector e(epsilon(rndGen));

    // Set intensities and length scales
    bool found = false;

    forAll(Gamma2, i)
    {
        // Random length scale ratio, gamma = sigmax/sigmay = sigmax/sigmaz
        // - using gamma^2 to ease lookup of c2 coefficient
        label g2 = Gamma2[i];

        if (setScales(sigmaX, g2, e, lambda, sigma_, alpha_, crr))
        {
            found = true;
            break;
        }
    }

    // Normalisation coefficient (eq. 11)
    // Note: sqrt(10*V)/sqrt(nEddy) applied outside when computing uDash
    c1_ = cmptAv(sigma_)/cmptProduct(sigma_)*cmptMin(sigma_);

    if (found)
    {
        // Shuffle the gamma^2 values
        rndGen.shuffle(Gamma2);
    }
    else
    {
        if (debug)
        {
            // If not found typically means that the stress has a repeated
            // eigenvalue/not covered by the selection of Gamma values, e.g.
            // as seen by range of applicability on Lumley diagram
            WarningInFunction
                << "Unable to set eddy intensity for eddy: " << *this
                << endl;
        }

        // Remove the influence of this eddy/indicate that its initialisation
        // failed
        patchFaceI_ = -1;
    }
}


Foam::myEddy::myEddy(const myEddy& e)
:
    patchFaceI_(e.patchFaceI_),
    position0_(e.position0_),
    x_(e.x_),
    sigma_(e.sigma_),
    alpha_(e.alpha_),
    Rpg_(e.Rpg_),
    c1_(e.c1_),
    dir1_(e.dir1_)
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

Foam::vector Foam::myEddy::uDash(const point& xp, const vector& n) const
{
    // Relative position inside eddy (global system)
    const vector r = cmptDivide(xp - position(n), sigma_);

    if (mag(r) > 1)
    {
        return vector::zero;
    }

    // Relative position inside eddy (eddy principal system)
    const vector rp = Rpg_.T() & r;

    // Shape function (eddy principal system)
    const vector q = cmptMultiply(sigma_, vector::one - cmptMultiply(rp, rp));

    // Fluctuating velocity (eddy principal system) (eq. 8)
    const vector uDashp = cmptMultiply(q, rp^alpha_);

    // Convert into global system (eq. 10)
    return c1_*(Rpg_ & uDashp);
}


void Foam::myEddy::writeCentreOBJ
(
    const vector& n,
    Ostream& os
) const
{
    point p = position(n);
    os  << "v " << p.x() << " " << p.y() << " " << p.z() << nl;
}


Foam::label Foam::myEddy::writeSurfaceOBJ
(
    const label pointOffset,
    const vector& n,
    Ostream& os
) const
{
    if (patchFaceI_ < 0)
    {
        // Invalid eddy
        return 0;
    }

    static const label nFaceAxis = 20;
    static const label nFaceTheta = 22;
    static const label nEddyPoints = (nFaceAxis - 1)*nFaceTheta + 2;
    static FixedList<point, nEddyPoints> x;

    static scalar dTheta = mathematical::twoPi/nFaceTheta;
    static scalar dPhi = mathematical::pi/scalar(nFaceAxis);

    label pointI = pointOffset;

    const vector& s = sigma_;

    const vector axisDir = tensor::I.vectorComponent(dir1_);
    const label dir2 = (dir1_ + 1) % 3;
    const label dir3 = (dir1_ + 2) % 3;

    // Calculate the point positions
    x[0] = axisDir*s[dir1_];
    x[nEddyPoints - 1] = - axisDir*s[dir1_];

    label eddyPtI = 1;
    for (label axisI = 1; axisI < nFaceAxis; axisI++)
    {
        scalar z = s[dir1_]*cos(axisI*dPhi);
        scalar r = sqrt(sqr(s[dir2])*(1 - sqr(z)/sqr(s[dir1_])));

        for (label thetaI = 0; thetaI < nFaceTheta; thetaI++)
        {
            scalar theta = thetaI*dTheta;
            point& p = x[eddyPtI++];
            p[dir1_] = z;
            p[dir2] = r*sin(theta);
            p[dir3] = r*cos(theta);
        }
    }

    // Write points
    forAll(x, i)
    {
        point p = position(n) + (Rpg_ & x[i]);
        os  << "v " << p.x() << " " << p.y() << " " << p.z() << nl;
    }

    // Write the end cap tri faces
    for (label faceI = 0; faceI < nFaceTheta; faceI++)
    {
        label p1 = pointI + 1;
        label p2 = p1 + faceI + 1;
        label p3 = p2 + 1;
        if (faceI == nFaceTheta - 1) p3 -= nFaceTheta;
        os  << "f " << p1 << " " << p2 << " " << p3 << nl;

        label q1 = pointI + nEddyPoints;
        label q2 = q1 - faceI - 1;
        label q3 = q2 - 1;
        if (faceI == nFaceTheta - 1) q3 += nFaceTheta;
        os  << "f " << q1 << " " << q2 << " " << q3 << nl;
    }

    // Write quad faces
    for (label axisI = 1; axisI < nFaceAxis - 1; axisI++)
    {
        for (label thetaI = 0; thetaI < nFaceTheta; thetaI++)
        {
            label p1 = pointI + 1 + (axisI - 1)*nFaceTheta + thetaI + 1;
            label p2 = p1 + nFaceTheta;
            label p3 = p2 + 1;
            label p4 = p1 + 1;

            if (thetaI == nFaceTheta - 1)
            {
                p3 -= nFaceTheta;
                p4 -= nFaceTheta;
            }
            os  << "f " << p1 << " " << p2 << " " << p3 << " " << p4 << nl;
        }
    }

    return nEddyPoints;
}


// ************************************************************************* //
