/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | www.openfoam.com
     \\/     M anipulation  |
-------------------------------------------------------------------------------
    Copyright (C) 2015 OpenFOAM Foundation
    Copyright (C) 2016-2019 OpenCFD Ltd.
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

#include "myDFSEMInletFvPatchVectorField.H"
#include "volFields.H"
#include "addToRunTimeSelectionTable.H"
#include "fvPatchFieldMapper.H"
#include "momentOfInertia.H"
#include "Fstream.H"
#include "globalIndex.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

Foam::label Foam::myDFSEMInletFvPatchVectorField::seedIterMax_ = 1000;

// * * * * * * * * * * * * * Private Member Functions  * * * * * * * * * * * //

void Foam::myDFSEMInletFvPatchVectorField::writeEddyOBJ() const
{
    {
        // Output the bounding box
        OFstream os(db().time().path()/"eddyBox.obj");

        const polyPatch& pp = this->patch().patch();
        const labelList& boundaryPoints = pp.boundaryPoints();
        const pointField& localPoints = pp.localPoints();

        vector offset = patchNormal_*maxSigmaX_;
        forAll(boundaryPoints, i)
        {
            point p = localPoints[boundaryPoints[i]];
            p += offset;
            os  << "v " << p.x() << " " << p.y() << " " << p.z() << nl;
        }

        forAll(boundaryPoints, i)
        {
            point p = localPoints[boundaryPoints[i]];
            p -= offset;
            os  << "v " << p.x() << " " << p.y() << " " << p.z() << nl;
        }

        // Draw lines between points
        // Note: need to order to avoid crossing patch
        //const label nPoint = boundaryPoints.size();
        //
        //forAll(boundaryPoints, i)
        //{
        //    label i1 = i;
        //    label i2 = (i + 1) % nPoint;
        //    os  << "l " << i1 << " " << i2 << nl;
        //}
        //
        //forAll(boundaryPoints, i)
        //{
        //    label i1 = i + nPoint;
        //    label i2 = ((i + 1) % nPoint) + nPoint;
        //    os  << "l " << i1 << " " << i2 << nl;
        //}
    }

    {
        const Time& time = db().time();
        OFstream os
        (
            time.path()/"eddies_" + Foam::name(time.timeIndex()) + ".obj"
        );

        label pointOffset = 0;
        forAll(eddies_, eddyI)
        {
            const myEddy& e = eddies_[eddyI];
            pointOffset += e.writeSurfaceOBJ(pointOffset, patchNormal_, os);
        }
    }
}


void Foam::myDFSEMInletFvPatchVectorField::writeLumleyCoeffs() const
{
    // Output list of xi vs eta

    // Before interpolation/raw data
    if (interpolateR_)
    {
        fileName valsFile
        (
            fileHandler().filePath
            (
                fileName
                (
                    db().time().path()
                   /db().time().caseConstant()
                   /"boundaryData"
                   /this->patch().name()
                   /"0"
                   /"R"
                )
            )
        );

        autoPtr<ISstream> isPtr
        (
            fileHandler().NewIFstream
            (
                valsFile
            )
        );

        Field<symmTensor> Rexp(isPtr());

        OFstream os(db().time().path()/"lumley_input.out");

        os  << "# xi" << token::TAB << "eta" << endl;

        forAll(Rexp, faceI)
        {
            // Normalised anisotropy tensor
            symmTensor devR = dev(Rexp[faceI]/(tr(Rexp[faceI])));

            // Second tensor invariant
            scalar ii = min(0, invariantII(devR));

            // Third tensor invariant
            scalar iii = invariantIII(devR);

            // xi, eta
            // See Pope - characterization of Reynolds-stress anisotropy
            scalar xi = cbrt(0.5*iii);
            scalar eta = sqrt(-ii/3.0);
            os  << xi << token::TAB << eta << token::TAB
                << ii << token::TAB << iii << endl;
        }
    }

    // After interpolation
    {
        OFstream os(db().time().path()/"lumley_interpolated.out");

        os  << "# xi" << token::TAB << "eta" << endl;

        forAll(R_, faceI)
        {
            // Normalised anisotropy tensor
            symmTensor devR = dev(R_[faceI]/(tr(R_[faceI])));

            // Second tensor invariant
            scalar ii = min(0, invariantII(devR));

            // Third tensor invariant
            scalar iii = invariantIII(devR);

            // xi, eta
            // See Pope - characterization of Reynolds-stress anisotropy
            scalar xi = cbrt(0.5*iii);
            scalar eta = sqrt(-ii/3.0);
            os  << xi << token::TAB << eta << token::TAB
                << ii << token::TAB << iii << endl;
        }
    }
}


const Foam::pointToPointPlanarInterpolation&
Foam::myDFSEMInletFvPatchVectorField::patchMapper() const
{
    // Initialise interpolation (2D planar interpolation by triangulation)
    if (mapperPtr_.empty())
    {
        // Reread values and interpolate
        fileName samplePointsFile
        (
            this->db().time().path()
           /this->db().time().caseConstant()
           /"boundaryData"
           /this->patch().name()
           /"points"
        );

        pointField samplePoints((IFstream(samplePointsFile)()));

        if (debug)
        {
            InfoInFunction
                << " Read " << samplePoints.size() << " sample points from "
                << samplePointsFile << endl;
        }


        // tbd: run-time selection
        bool nearestOnly =
        (
           !mapMethod_.empty()
         && mapMethod_ != "planarInterpolation"
        );

        // Allocate the interpolator
        mapperPtr_.reset
        (
            new pointToPointPlanarInterpolation
            (
                samplePoints,
                this->patch().patch().faceCentres(),
                perturb_,
                nearestOnly
            )
        );
    }

    return *mapperPtr_;
}


void Foam::myDFSEMInletFvPatchVectorField::initialisePatch()
{
    const vectorField nf(patch().nf());

    // Patch normal points into domain
    patchNormal_ = -gAverage(nf);

    // Check that patch is planar
    scalar error = max(magSqr(patchNormal_ + nf));

    if (error > SMALL)
    {
        WarningInFunction
            << "Patch " << patch().name() << " is not planar"
            << endl;
    }

    patchNormal_ /= mag(patchNormal_) + ROOTVSMALL;


    // Decompose the patch faces into triangles to enable point search

    const polyPatch& patch = this->patch().patch();
    const pointField& points = patch.points();

    // Triangulate the patch faces and create addressing
    DynamicList<label> triToFace(2*patch.size());
    DynamicList<scalar> triMagSf(2*patch.size());
    DynamicList<face> triFace(2*patch.size());
    DynamicList<face> tris(5);

    // Set zero value at the start of the tri area list
    triMagSf.append(0.0);

    forAll(patch, faceI)
    {
        const face& f = patch[faceI];

        tris.clear();
        f.triangles(points, tris);

        forAll(tris, i)
        {
            triToFace.append(faceI);
            triFace.append(tris[i]);
            triMagSf.append(tris[i].mag(points));
        }
    }

    forAll(sumTriMagSf_, i)
    {
        sumTriMagSf_[i] = 0.0;
    }

    sumTriMagSf_[Pstream::myProcNo() + 1] = sum(triMagSf);

    Pstream::listCombineGather(sumTriMagSf_, maxEqOp<scalar>());
    Pstream::listCombineScatter(sumTriMagSf_);

    for (label i = 1; i < triMagSf.size(); i++)
    {
        triMagSf[i] += triMagSf[i-1];
    }

    // Transfer to persistent storage
    triFace_.transfer(triFace);
    triToFace_.transfer(triToFace);
    triCumulativeMagSf_.transfer(triMagSf);

    // Convert sumTriMagSf_ into cumulative sum of areas per proc
    for (label i = 1; i < sumTriMagSf_.size(); i++)
    {
        sumTriMagSf_[i] += sumTriMagSf_[i-1];
    }

    // Global patch area (over all processors)
    patchArea_ = sumTriMagSf_.last();

    // Local patch bounds (this processor)
    patchBounds_ = boundBox(patch.localPoints(), false);
    patchBounds_.inflate(0.1);

    // Determine if all eddies spawned from a single processor
    singleProc_ = patch.size() == returnReduce(patch.size(), sumOp<label>());
    reduce(singleProc_, orOp<bool>());
}


void Foam::myDFSEMInletFvPatchVectorField::initialiseEddyBox()
{
    const scalarField& magSf = patch().magSf();

    // const scalarField cellDx(Foam::sqrt(magSf));
    const scalarField cellDx(max(Foam::sqrt(magSf), 2/patch().deltaCoeffs()));
    // Info << "max(Foam::sqrt(magSf))" << max(Foam::sqrt(magSf)) << endl;
    // Info << "2/patch().deltaCoeffs()" << max(2/patch().deltaCoeffs()) << endl;

    // Inialise eddy box extents
    forAll(*this, faceI)
    {
        scalar& s = sigmax_[faceI];

        // Length scale in x direction (based on eq. 14)
        s = mag(L_[faceI]);
        s = min(s, kappa_*delta_);

        // Allow eddies to be smaller than the mesh scale as suggested by
        // the reference?
        // s = min(s, nCellPerEddy_*cellDx[faceI]);
        s = max(s, nCellPerEddy_*cellDx[faceI]);
    }

    // Maximum extent across all processors
    maxSigmaX_ = gMax(sigmax_);

    // Eddy box volume
    v0_ = 2*gSum(magSf)*maxSigmaX_;

    if (debug)
    {
        Info<< "Patch: " << patch().patch().name() << " eddy box:" << nl
            << "    volume    : " << v0_ << nl
            << "    maxSigmaX : " << maxSigmaX_ << nl
            << endl;
    }
}


Foam::pointIndexHit Foam::myDFSEMInletFvPatchVectorField::setNewPosition
(
    const bool global
)
{
    // Initialise to miss
    pointIndexHit pos(false, vector::max, -1);

    const polyPatch& patch = this->patch().patch();
    const pointField& points = patch.points();

    if (global)
    {
        scalar areaFraction = rndGen_.globalPosition<scalar>(0, patchArea_);

        // Determine which processor to use
        label procI = 0;
        forAllReverse(sumTriMagSf_, i)
        {
            if (areaFraction >= sumTriMagSf_[i])
            {
                procI = i;
                break;
            }
        }

        if (Pstream::myProcNo() == procI)
        {
            // Find corresponding decomposed face triangle
            label triI = 0;
            scalar offset = sumTriMagSf_[procI];
            forAllReverse(triCumulativeMagSf_, i)
            {
                if (areaFraction > triCumulativeMagSf_[i] + offset)
                {
                    triI = i;
                    break;
                }
            }

            // Find random point in triangle
            const face& tf = triFace_[triI];
            const triPointRef tri(points[tf[0]], points[tf[1]], points[tf[2]]);

            pos.setHit();
            pos.setIndex(triToFace_[triI]);
            pos.rawPoint() = tri.randomPoint(rndGen_);
        }
    }
    else
    {
        // Find corresponding decomposed face triangle on local processor
        label triI = 0;
        scalar maxAreaLimit = triCumulativeMagSf_.last();
        scalar areaFraction = rndGen_.position<scalar>(0, maxAreaLimit);

        forAllReverse(triCumulativeMagSf_, i)
        {
            if (areaFraction > triCumulativeMagSf_[i])
            {
                triI = i;
                break;
            }
        }

        // Find random point in triangle
        const face& tf = triFace_[triI];
        const triPointRef tri(points[tf[0]], points[tf[1]], points[tf[2]]);

        pos.setHit();
        pos.setIndex(triToFace_[triI]);
        pos.rawPoint() = tri.randomPoint(rndGen_);
    }

    return pos;
}


void Foam::myDFSEMInletFvPatchVectorField::initialiseEddies()
{
    DynamicList<myEddy> eddies(size());

    // Initialise eddy properties
    scalar sumVolEddy = 0;
    scalar sumVolEddyAllProc = 0;

    while (sumVolEddyAllProc/v0_ < d_)
    {
        bool search = true;
        label iter = 0;

        while (search && iter++ < seedIterMax_)
        {
            // Get new parallel consistent position
            pointIndexHit pos(setNewPosition(true));
            label faceI = pos.index();

            // Note: only 1 processor will pick up this face
            if (faceI != -1)
            {
                myEddy e
                (
                    faceI,
                    pos.hitPoint(),
                    rndGen_.position<scalar>(-maxSigmaX_, maxSigmaX_),
                    sigmax_[faceI],
                    R_[faceI],
                    rndGen_,
                    crr_
                );
                
                // If eddy valid, patchFaceI is non-zero
                if (e.patchFaceI() != -1)
                {
                    eddies.append(e);
                    sumVolEddy += e.volume();
                    search = false;
                }
            }
            // else eddy on remote processor

            reduce(search, andOp<bool>());
        }


        sumVolEddyAllProc = returnReduce(sumVolEddy, sumOp<scalar>());
    }
    eddies_.transfer(eddies);

    nEddy_ = eddies_.size();

    if (debug)
    {
        Pout<< "Patch:" << patch().patch().name();

        if (Pstream::parRun())
        {
            Pout<< " processor:" << Pstream::myProcNo();
        }

        Pout<< " seeded:" << nEddy_ << " eddies" << endl;
    }

    reduce(nEddy_, sumOp<label>());

    if (nEddy_ > 0)
    {
        Info<< "Eddy Scaling constant k0 is: " << crr_ << endl;
        Info<< "Turbulent DFSEM patch: " << patch().name()
            << " seeded " << nEddy_ << " eddies with total volume "
            << sumVolEddyAllProc
            << endl;
    }
    else
    {
        WarningInFunction
            << "Patch: " << patch().patch().name()
            << " on field " << internalField().name()
            << ": No eddies seeded - please check your set-up" << endl;
    }
}


void Foam::myDFSEMInletFvPatchVectorField::convectEddies
(
    const scalar deltaT
)
{
    // Note: all operations applied to local processor only

    label nRecycled = 0;

    forAll(eddies_, eddyI)
    {
        myEddy& e = eddies_[eddyI];
        e.move(deltaT*(UMean_ & patchNormal_));

        const scalar position0 = e.x();

        // Check to see if eddy has exited downstream box plane
        if (position0 > maxSigmaX_)
        {
            bool search = true;
            label iter = 0;

            while (search && iter++ < seedIterMax_)
            {
               // Spawn new eddy with new random properties (intensity etc)
               pointIndexHit pos(setNewPosition(false));
               label faceI = pos.index();

               e = myEddy
                    (
                        faceI,
                        pos.hitPoint(),
                        position0 - floor(position0/maxSigmaX_)*maxSigmaX_,
                        sigmax_[faceI],
                        R_[faceI],
                        rndGen_,
                        crr_
                    );

                if (e.patchFaceI() != -1)
                {
                    search = false;
                }
            }

            nRecycled++;
        }
    }

    reduce(nRecycled, sumOp<label>());

    if (debug && nRecycled > 0)
    {
        Info<< "Patch: " << patch().patch().name() << " recycled "
            << nRecycled << " eddies" << endl;
    }
}


Foam::vector Foam::myDFSEMInletFvPatchVectorField::uDashEddy
(
    const List<myEddy>& eddies,
    const point& patchFaceCf
) const
{
    vector uDash(Zero);

    forAll(eddies, k)
    {
        const myEddy& e = eddies[k];
        uDash += e.uDash(patchFaceCf, patchNormal_);
    }

    return uDash;
}


void Foam::myDFSEMInletFvPatchVectorField::calcOverlappingProcEddies
(
    List<List<myEddy>>& overlappingEddies
) const
{
    int oldTag = UPstream::msgType();
    UPstream::msgType() = oldTag + 1;

    List<boundBox> patchBBs(Pstream::nProcs());
    patchBBs[Pstream::myProcNo()] = patchBounds_;
    Pstream::gatherList(patchBBs);
    Pstream::scatterList(patchBBs);

    // Per processor indices into all segments to send
    List<DynamicList<label>> dynSendMap(Pstream::nProcs());

    forAll(eddies_, i)
    {
        // Collect overlapping eddies
        const myEddy& e = eddies_[i];

        // Eddy bounds
        point x = e.position(patchNormal_);
        boundBox ebb = e.bounds();
        ebb.min() += x;
        ebb.max() += x;

        forAll(patchBBs, procI)
        {
            // Not including intersection with local patch
            if (procI != Pstream::myProcNo())
            {
                if (ebb.overlaps(patchBBs[procI]))
                {
                    dynSendMap[procI].append(i);
                }
            }
        }
    }

    labelListList sendMap(Pstream::nProcs());
    forAll(sendMap, procI)
    {
        sendMap[procI].transfer(dynSendMap[procI]);
    }

    // Send the number of eddies for local processors to receive
    labelListList sendSizes(Pstream::nProcs());
    sendSizes[Pstream::myProcNo()].setSize(Pstream::nProcs());
    forAll(sendMap, procI)
    {
        sendSizes[Pstream::myProcNo()][procI] = sendMap[procI].size();
    }
    Pstream::gatherList(sendSizes);
    Pstream::scatterList(sendSizes);

    // Determine order of receiving
    labelListList constructMap(Pstream::nProcs());

    // Local segment first
    constructMap[Pstream::myProcNo()] = identity
    (
        sendMap[Pstream::myProcNo()].size()
    );

    label segmentI = constructMap[Pstream::myProcNo()].size();
    forAll(constructMap, procI)
    {
        if (procI != Pstream::myProcNo())
        {
            // What I need to receive is what other processor is sending to me
            label nRecv = sendSizes[procI][Pstream::myProcNo()];
            constructMap[procI].setSize(nRecv);

            for (label i = 0; i < nRecv; i++)
            {
                constructMap[procI][i] = segmentI++;
            }
        }
    }

    mapDistribute map(segmentI, std::move(sendMap), std::move(constructMap));

    PstreamBuffers pBufs(Pstream::commsTypes::nonBlocking);

    for (label domain = 0; domain < Pstream::nProcs(); domain++)
    {
        const labelList& sendElems = map.subMap()[domain];

        if (domain != Pstream::myProcNo() && sendElems.size())
        {
            List<myEddy> subEddies(UIndirectList<myEddy>(eddies_, sendElems));

            UOPstream toDomain(domain, pBufs);

            toDomain<< subEddies;
        }
    }

    // Start receiving
    pBufs.finishedSends();

    // Consume
    for (label domain = 0; domain < Pstream::nProcs(); domain++)
    {
        const labelList& recvElems = map.constructMap()[domain];

        if (domain != Pstream::myProcNo() && recvElems.size())
        {
            UIPstream str(domain, pBufs);
            {
                str >> overlappingEddies[domain];
            }
        }
    }

    // Restore tag
    UPstream::msgType() = oldTag;
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::myDFSEMInletFvPatchVectorField::
myDFSEMInletFvPatchVectorField
(
    const fvPatch& p,
    const DimensionedField<vector, volMesh>& iF
)
:
    fixedValueFvPatchField<vector>(p, iF),
    delta_(Zero),
    d_(Zero),
    kappa_(Zero),
    crr_(Zero),

    perturb_(1e-5),
    mapMethod_("nearestCell"),
    mapperPtr_(nullptr),
    interpolateR_(false),
    interpolateL_(false),
    interpolateU_(false),
    R_(),
    L_(),
    U_(),
    UMean_(Zero),

    patchArea_(-1),
    triFace_(),
    triToFace_(),
    triCumulativeMagSf_(),
    sumTriMagSf_(Pstream::nProcs() + 1, Zero),

    eddies_(Zero),
    nCellPerEddy_(5),
    patchNormal_(Zero),
    v0_(Zero),
    rndGen_(Pstream::myProcNo()),
    sigmax_(size(), Zero),
    maxSigmaX_(Zero),
    nEddy_(Zero),
    curTimeIndex_(-1),
    patchBounds_(boundBox::invertedBox),
    singleProc_(false),
    writeEddies_(false)
{}


Foam::myDFSEMInletFvPatchVectorField::
myDFSEMInletFvPatchVectorField
(
    const myDFSEMInletFvPatchVectorField& ptf,
    const fvPatch& p,
    const DimensionedField<vector, volMesh>& iF,
    const fvPatchFieldMapper& mapper
)
:
    fixedValueFvPatchField<vector>(ptf, p, iF, mapper),
    delta_(ptf.delta_),
    d_(ptf.d_),
    kappa_(ptf.kappa_),
    crr_(ptf.crr_),

    perturb_(ptf.perturb_),
    mapMethod_(ptf.mapMethod_),
    mapperPtr_(nullptr),
    interpolateR_(ptf.interpolateR_),
    interpolateL_(ptf.interpolateL_),
    interpolateU_(ptf.interpolateU_),
    R_(ptf.R_, mapper),
    L_(ptf.L_, mapper),
    U_(ptf.U_, mapper),
    UMean_(ptf.UMean_),

    patchArea_(ptf.patchArea_),
    triFace_(ptf.triFace_),
    triToFace_(ptf.triToFace_),
    triCumulativeMagSf_(ptf.triCumulativeMagSf_),
    sumTriMagSf_(ptf.sumTriMagSf_),

    eddies_(ptf.eddies_),
    nCellPerEddy_(ptf.nCellPerEddy_),
    patchNormal_(ptf.patchNormal_),
    v0_(ptf.v0_),
    rndGen_(ptf.rndGen_),
    sigmax_(ptf.sigmax_, mapper),
    maxSigmaX_(ptf.maxSigmaX_),
    nEddy_(Zero),
    curTimeIndex_(-1),
    patchBounds_(ptf.patchBounds_),
    singleProc_(ptf.singleProc_),
    writeEddies_(ptf.writeEddies_)
{}


Foam::myDFSEMInletFvPatchVectorField::
myDFSEMInletFvPatchVectorField
(
    const fvPatch& p,
    const DimensionedField<vector, volMesh>& iF,
    const dictionary& dict
)
:
    fixedValueFvPatchField<vector>(p, iF, dict),
    delta_(dict.get<scalar>("delta")),
    d_(dict.getOrDefault<scalar>("d", 1.0)),
    kappa_(dict.getOrDefault<scalar>("kappa", 0.41)),
    crr_(dict.getOrDefault<scalar>("k0", 8e-4)),

    perturb_(dict.getOrDefault<scalar>("perturb", 1e-5)),
    mapMethod_(dict.getOrDefault<word>("mapMethod", "nearestCell")),
    mapperPtr_(nullptr),
    interpolateR_(dict.getOrDefault<bool>("interpolateR", false)),
    interpolateL_(dict.getOrDefault<bool>("interpolateL", false)),
    interpolateU_(dict.getOrDefault<bool>("interpolateU", false)),
    R_(interpolateOrRead<symmTensor>("R", dict, interpolateR_)),
    L_(interpolateOrRead<scalar>("L", dict, interpolateL_)),
    U_(interpolateOrRead<vector>("U", dict, interpolateU_)),
    UMean_(Zero),

    patchArea_(-1),
    triFace_(),
    triToFace_(),
    triCumulativeMagSf_(),
    sumTriMagSf_(Pstream::nProcs() + 1, Zero),

    eddies_(),
    nCellPerEddy_(dict.getOrDefault<label>("nCellPerEddy", 5)),
    patchNormal_(Zero),
    v0_(Zero),
    rndGen_(),
    sigmax_(size(), Zero),
    maxSigmaX_(Zero),
    nEddy_(Zero),
    curTimeIndex_(-1),
    patchBounds_(boundBox::invertedBox),
    singleProc_(false),
    writeEddies_(dict.getOrDefault<bool>("writeEddies", false))
{
    myEddy::debug = debug;

    checkStresses(R_);

    // Set UMean as patch area average value
    UMean_ = gSum(U_*patch().magSf())/(gSum(patch().magSf()) + ROOTVSMALL);
}


Foam::myDFSEMInletFvPatchVectorField::
myDFSEMInletFvPatchVectorField
(
    const myDFSEMInletFvPatchVectorField& ptf
)
:
    fixedValueFvPatchField<vector>(ptf),
    delta_(ptf.delta_),
    d_(ptf.d_),
    kappa_(ptf.kappa_),
    crr_(ptf.crr_),

    perturb_(ptf.perturb_),
    mapMethod_(ptf.mapMethod_),
    mapperPtr_(nullptr),
    interpolateR_(ptf.interpolateR_),
    interpolateL_(ptf.interpolateL_),
    interpolateU_(ptf.interpolateU_),
    R_(ptf.R_),
    L_(ptf.L_),
    U_(ptf.U_),
    UMean_(ptf.UMean_),

    patchArea_(ptf.patchArea_),
    triFace_(ptf.triFace_),
    triToFace_(ptf.triToFace_),
    triCumulativeMagSf_(ptf.triCumulativeMagSf_),
    sumTriMagSf_(ptf.sumTriMagSf_),

    eddies_(ptf.eddies_),
    nCellPerEddy_(ptf.nCellPerEddy_),
    patchNormal_(ptf.patchNormal_),
    v0_(ptf.v0_),
    rndGen_(ptf.rndGen_),
    sigmax_(ptf.sigmax_),
    maxSigmaX_(ptf.maxSigmaX_),
    nEddy_(Zero),
    curTimeIndex_(-1),
    patchBounds_(ptf.patchBounds_),
    singleProc_(ptf.singleProc_),
    writeEddies_(ptf.writeEddies_)
{}


Foam::myDFSEMInletFvPatchVectorField::
myDFSEMInletFvPatchVectorField
(
    const myDFSEMInletFvPatchVectorField& ptf,
    const DimensionedField<vector, volMesh>& iF
)
:
    fixedValueFvPatchField<vector>(ptf, iF),
    delta_(ptf.delta_),
    d_(ptf.d_),
    kappa_(ptf.kappa_),
    crr_(ptf.crr_),

    perturb_(ptf.perturb_),
    mapMethod_(ptf.mapMethod_),
    mapperPtr_(nullptr),
    interpolateR_(ptf.interpolateR_),
    interpolateL_(ptf.interpolateL_),
    interpolateU_(ptf.interpolateU_),
    R_(ptf.R_),
    L_(ptf.L_),
    U_(ptf.U_),
    UMean_(ptf.UMean_),

    patchArea_(ptf.patchArea_),
    triFace_(ptf.triFace_),
    triToFace_(ptf.triToFace_),
    triCumulativeMagSf_(ptf.triCumulativeMagSf_),
    sumTriMagSf_(ptf.sumTriMagSf_),

    eddies_(ptf.eddies_),
    nCellPerEddy_(ptf.nCellPerEddy_),
    patchNormal_(ptf.patchNormal_),
    v0_(ptf.v0_),
    rndGen_(ptf.rndGen_),
    sigmax_(ptf.sigmax_),
    maxSigmaX_(ptf.maxSigmaX_),
    nEddy_(Zero),
    curTimeIndex_(-1),
    patchBounds_(ptf.patchBounds_),
    singleProc_(ptf.singleProc_),
    writeEddies_(ptf.writeEddies_)
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

bool Foam::myDFSEMInletFvPatchVectorField::checkStresses
(
    const symmTensorField& Rf
)
{
    // Perform checks of the stress tensor based on Cholesky decomposition
    // constraints

    forAll(Rf, facei)
    {
        const symmTensor& R = Rf[facei];

        if (R.xx() <= 0)
        {
            FatalErrorInFunction
                << "Reynolds stress " << R << " at face " << facei
                << " does not obey the constraint: R_xx > 0"
                << exit(FatalError);
        }

        scalar a_xx = sqrt(R.xx());

        scalar a_xy = R.xy()/a_xx;

        scalar a_yy_2 = R.yy() - sqr(a_xy);

        if (a_yy_2 < 0)
        {
            FatalErrorInFunction
                << "Reynolds stress " << R << " at face " << facei
                << " leads to an invalid Cholesky decomposition due to the "
                << "constraint R_yy - sqr(a_xy) >= 0"
                << exit(FatalError);
        }

        scalar a_yy = Foam::sqrt(a_yy_2);

        scalar a_xz = R.xz()/a_xx;

        scalar a_yz = (R.yz() - a_xy*a_xz)*a_yy;

        scalar a_zz_2 = R.zz() - sqr(a_xz) - sqr(a_yz);

        if (a_zz_2 < 0)
        {
            FatalErrorInFunction
                << "Reynolds stress " << R << " at face " << facei
                << " leads to an invalid Cholesky decomposition due to the "
                << "constraint R_zz - sqr(a_xz) - sqr(a_yz) >= 0"
                << exit(FatalError);
        }

        scalar a_zz = Foam::sqrt(a_zz_2);

        if (debug)
        {
            Pout<< "R: " << R
                << " a_xx:" << a_xx << " a_xy:" << a_xy << " a_xz:" << a_xy
                <<                     " a_yy:" << a_yy << " a_yz:" << a_yz
                <<                                         " a_zz:" << a_zz
                << endl;
        }
    }

    return true;
}


void Foam::myDFSEMInletFvPatchVectorField::autoMap
(
    const fvPatchFieldMapper& m
)
{
    fixedValueFvPatchField<vector>::autoMap(m);

    // Clear interpolator
    mapperPtr_.clear();
    R_.autoMap(m);
    L_.autoMap(m);
    U_.autoMap(m);

    sigmax_.autoMap(m);
}


void Foam::myDFSEMInletFvPatchVectorField::rmap
(
    const fvPatchVectorField& ptf,
    const labelList& addr
)
{
    fixedValueFvPatchField<vector>::rmap(ptf, addr);

    const myDFSEMInletFvPatchVectorField& dfsemptf =
        refCast<const myDFSEMInletFvPatchVectorField>(ptf);

    R_.rmap(dfsemptf.R_, addr);
    L_.rmap(dfsemptf.L_, addr);
    U_.rmap(dfsemptf.U_, addr);

    // Clear interpolator
    mapperPtr_.clear();

    sigmax_.rmap(dfsemptf.sigmax_, addr);
}


void Foam::myDFSEMInletFvPatchVectorField::updateCoeffs()
{
    if (updated())
    {
        return;
    }

    if (curTimeIndex_ == -1)
    {
        initialisePatch();

        initialiseEddyBox();

        initialiseEddies();
    }


    if (curTimeIndex_ != db().time().timeIndex())
    {
        if (debug)
        {
            label n = eddies_.size();
            Info<< "Number of eddies: " << returnReduce(n, sumOp<label>())
                << endl;
        }

        const scalar deltaT = db().time().deltaTValue();

        // Move eddies using mean velocity
        convectEddies(deltaT);

        // Set velocity
        vectorField& U = *this;
        //U = UMean_;
        U = U_;

        const pointField& Cf = patch().Cf();

        // Apply second part of normalisation coefficient
        // Note: factor of 2 required to match reference stresses?
        const scalar FACTOR = 2;
        const scalar c = FACTOR*Foam::sqrt(10*v0_)/Foam::sqrt(scalar(nEddy_));

        // In parallel, need to collect all eddies that will interact with
        // local faces

        if (singleProc_ || !Pstream::parRun())
        {
            forAll(U, faceI)
            {
                U[faceI] += c*uDashEddy(eddies_, Cf[faceI]);
            }
        }
        else
        {
            // Process local eddy contributions
            forAll(U, faceI)
            {
                U[faceI] += c*uDashEddy(eddies_, Cf[faceI]);
            }

            // Add contributions from overlapping eddies
            List<List<myEddy>> overlappingEddies(Pstream::nProcs());
            calcOverlappingProcEddies(overlappingEddies);

            forAll(overlappingEddies, procI)
            {
                const List<myEddy>& eddies = overlappingEddies[procI];

                if (eddies.size())
                {
                    //Pout<< "Applying " << eddies.size()
                    //    << " eddies from processor " << procI << endl;

                    forAll(U, faceI)
                    {
                        U[faceI] += c*uDashEddy(eddies, Cf[faceI]);
                    }
                }
            }
        }

        // Re-scale to ensure correct flow rate
        scalar fCorr =
            gSum((UMean_ & patchNormal_)*patch().magSf())
           /gSum(U & -patch().Sf());

        U *= fCorr;

        if (debug)
        {
            Info<< "Patch:" << patch().patch().name()
                << " min/max(U):" << gMin(U) << ", " << gMax(U) << endl;
        }

        curTimeIndex_ = db().time().timeIndex();

        if (writeEddies_)
        {
            writeEddyOBJ();
        }

        if (debug && db().time().writeTime())
        {
            writeLumleyCoeffs();
        }
    }

    fixedValueFvPatchVectorField::updateCoeffs();
}


void Foam::myDFSEMInletFvPatchVectorField::write(Ostream& os) const
{
    fvPatchField<vector>::write(os);
    writeEntry("value", os);
    os.writeEntry("delta", delta_);
    os.writeEntryIfDifferent<scalar>("d", 1.0, d_);
    os.writeEntryIfDifferent<scalar>("kappa", 0.41, kappa_);
    os.writeEntryIfDifferent<scalar>("perturb", 1e-5, perturb_);
    os.writeEntryIfDifferent<label>("nCellPerEddy", 5, nCellPerEddy_);
    os.writeEntryIfDifferent("writeEddies", false, writeEddies_);

    if (!interpolateR_)
    {
        R_.writeEntry("R", os);
    }

    if (!interpolateL_)
    {
        L_.writeEntry("L", os);
    }

    if (!interpolateU_)
    {
        U_.writeEntry("U", os);
    }

    if (!mapMethod_.empty())
    {
        os.writeEntryIfDifferent<word>
        (
            "mapMethod",
            "nearestCell",
            mapMethod_
        );
    }
}


// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

namespace Foam
{
   makePatchTypeField
   (
       fvPatchVectorField,
       myDFSEMInletFvPatchVectorField
   );
}


// ************************************************************************* //
