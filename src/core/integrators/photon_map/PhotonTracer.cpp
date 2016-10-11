#include "PhotonTracer.hpp"

#include "bvh/BinaryBvh.hpp"

namespace Tungsten {

PhotonTracer::PhotonTracer(TraceableScene *scene, const PhotonMapSettings &settings, uint32 threadId)
: TraceBase(scene, settings, threadId),
  _settings(settings),
  _photonQuery(new const Photon *[settings.gatherCount]),
  _distanceQuery(new float[settings.gatherCount])
{
}

void PhotonTracer::tracePhoton(SurfacePhotonRange &surfaceRange, VolumePhotonRange &volumeRange,
        PathPhotonRange &pathRange, PathSampleGenerator &sampler)
{
    float lightPdf;
    const Primitive *light = chooseLightAdjoint(sampler, lightPdf);
    const Medium *medium = light->extMedium().get();

    PositionSample point;
    if (!light->samplePosition(sampler, point))
        return;
    DirectionSample direction;
    if (!light->sampleDirection(sampler, point, direction))
        return;

    Ray ray(point.p, direction.d);
    Vec3f throughput(point.weight*direction.weight/lightPdf);

    SurfaceScatterEvent event;
    IntersectionTemporary data;
    IntersectionInfo info;
    Medium::MediumState state;
    state.reset();
    Vec3f emission(0.0f);

    if (!pathRange.full()) {
        PathPhoton &p = pathRange.addPhoton();
        p.pos = point.p;
        p.power = throughput;
        p.setPathInfo(0, false);
    }

    int bounce = 0;
    bool wasSpecular = true;
    bool hitSurface = true;
    bool didHit = _scene->intersect(ray, data, info);
    while ((didHit || medium) && bounce < _settings.maxBounces - 1) {
        bounce++;

        if (medium) {
            MediumSample mediumSample;
            if (!medium->sampleDistance(sampler, ray, state, mediumSample))
                break;
            throughput *= mediumSample.weight;
            hitSurface = mediumSample.exited;

            if (!hitSurface) {
                if (!volumeRange.full()) {
                    VolumePhoton &p = volumeRange.addPhoton();
                    p.pos = mediumSample.p;
                    p.dir = ray.dir();
                    p.power = throughput;
                    p.bounce = bounce;
                }
                if (!pathRange.full()) {
                    PathPhoton &p = pathRange.addPhoton();
                    p.pos = mediumSample.p;
                    p.power = throughput;
                    p.setPathInfo(bounce, false);
                }

                PhaseSample phaseSample;
                if (!mediumSample.phase->sample(sampler, ray.dir(), phaseSample))
                    break;
                ray = ray.scatter(mediumSample.p, phaseSample.w, 0.0f);
                ray.setPrimaryRay(false);
                throughput *= phaseSample.weight;
            }
        }

        if (hitSurface) {
            if (!info.bsdf->lobes().isPureSpecular() && !surfaceRange.full()) {
                Photon &p = surfaceRange.addPhoton();
                p.pos = info.p;
                p.dir = ray.dir();
                p.power = throughput*std::abs(info.Ns.dot(ray.dir())/info.Ng.dot(ray.dir()));
                p.bounce = bounce;
            }
            if (!pathRange.full()) {
                PathPhoton &p = pathRange.addPhoton();
                p.pos = info.p;
                p.power = throughput;
                p.setPathInfo(bounce, false);
            }
        }

        if (volumeRange.full() && surfaceRange.full() && pathRange.full())
            break;

        if (hitSurface) {
            event = makeLocalScatterEvent(data, info, ray, &sampler);
            if (!handleSurface(event, data, info, medium, bounce,
                    true, false, ray, throughput, emission, wasSpecular, state))
                break;
        }

        if (throughput.max() == 0.0f)
            break;

        if (std::isnan(ray.dir().sum() + ray.pos().sum()))
            break;
        if (std::isnan(throughput.sum()))
            break;

        if (bounce < _settings.maxBounces)
            didHit = _scene->intersect(ray, data, info);
    }
}

Vec3f PhotonTracer::traceSample(Vec2u pixel, const KdTree<Photon> &surfaceTree,
        const KdTree<VolumePhoton> *mediumTree, const Bvh::BinaryBvh *beamBvh,
        const PathPhoton *pathPhotons, PathSampleGenerator &sampler,
        float gatherRadius, float volumeGatherRadius)
{
    PositionSample point;
    if (!_scene->cam().samplePosition(sampler, point))
        return Vec3f(0.0f);
    DirectionSample direction;
    if (!_scene->cam().sampleDirection(sampler, point, pixel, direction))
        return Vec3f(0.0f);

    Vec3f throughput = point.weight*direction.weight;
    Ray ray(point.p, direction.d);
    ray.setPrimaryRay(true);

    IntersectionTemporary data;
    IntersectionInfo info;
    const Medium *medium = _scene->cam().medium().get();

    Vec3f result(0.0f);
    int bounce = 0;
    bool didHit = _scene->intersect(ray, data, info);
    while ((medium || didHit) && bounce < _settings.maxBounces) {
        bounce++;

        if (medium) {
            if (mediumTree) {
                Vec3f beamEstimate(0.0f);
                mediumTree->beamQuery(ray.pos(), ray.dir(), ray.farT(), [&](const VolumePhoton &p, float t, float distSq) {
                    int fullPathBounce = bounce + p.bounce - 1;
                    if (fullPathBounce < _settings.minBounces || fullPathBounce >= _settings.maxBounces)
                        return;

                    Ray mediumQuery(ray);
                    mediumQuery.setFarT(t);
                    beamEstimate += (3.0f*INV_PI*sqr(1.0f - distSq/p.radiusSq))/p.radiusSq
                            *medium->phaseFunction(p.pos)->eval(ray.dir(), -p.dir)
                            *medium->transmittance(sampler, mediumQuery)*p.power;
                });
                result += throughput*beamEstimate;
            } else if (beamBvh) {
                Vec3f beamEstimate(0.0f);
                beamBvh->trace(ray, [&](Ray &ray, uint32 photonIndex, float /*tMin*/, const Vec3pf &bounds) {
                    const PathPhoton &p0 = pathPhotons[photonIndex + 0];
                    const PathPhoton &p1 = pathPhotons[photonIndex + 1];
                    int fullPathBounce = bounce + p0.bounce();
                    if (fullPathBounce < _settings.minBounces || fullPathBounce >= _settings.maxBounces)
                        return;

                    Vec3f u = ray.dir().cross(p0.dir);
                    float invSinTheta = 1.0f/u.length();

                    Vec3f l = p0.pos - ray.pos();
                    float d = invSinTheta*(u.dot(l));
                    if (std::abs(d) > volumeGatherRadius)
                        return;

                    Vec3f n = p0.dir.cross(u);
                    float t = n.dot(l)/n.dot(ray.dir());

                    int majorAxis = std::abs(p0.dir).maxDim();
                    float intervalMin = min(bounds[majorAxis][0], bounds[majorAxis][1]);
                    float intervalMax = max(bounds[majorAxis][2], bounds[majorAxis][3]);

                    Vec3f hitPoint = ray.pos() + ray.dir()*t;
                    if (hitPoint[majorAxis] < intervalMin || hitPoint[majorAxis] > intervalMax)
                        return;

                    float s = p0.dir.dot(hitPoint - p0.pos);
                    if (t >= ray.nearT() && t <= ray.farT() && s >= 0.0f && s <= p0.length) {
                        Ray mediumQuery(ray);
                        mediumQuery.setFarT(t);
                        beamEstimate += medium->sigmaT(hitPoint)*invSinTheta/(2.0f*volumeGatherRadius)
                                *medium->phaseFunction(hitPoint)->eval(ray.dir(), -p0.dir)
                                *medium->transmittance(sampler, mediumQuery)*p1.power;
                    }
                });
                result += throughput*beamEstimate;
            }
            throughput *= medium->transmittance(sampler, ray);
        }
        if (!didHit)
            break;

        const Bsdf &bsdf = *info.bsdf;

        SurfaceScatterEvent event = makeLocalScatterEvent(data, info, ray, &sampler);

        Vec3f transparency = bsdf.eval(event.makeForwardEvent(), false);
        float transparencyScalar = transparency.avg();

        Vec3f wo;
        if (sampler.nextBoolean(transparencyScalar)) {
            wo = ray.dir();
            throughput *= transparency/transparencyScalar;
        } else {
            event.requestedLobe = BsdfLobes::SpecularLobe;
            if (!bsdf.sample(event, false))
                break;

            wo = event.frame.toGlobal(event.wo);

            throughput *= event.weight;
        }

        bool geometricBackside = (wo.dot(info.Ng) < 0.0f);
        medium = info.primitive->selectMedium(medium, geometricBackside);

        ray = ray.scatter(ray.hitpoint(), wo, info.epsilon);

        if (std::isnan(ray.dir().sum() + ray.pos().sum()))
            break;
        if (std::isnan(throughput.sum()))
            break;

        if (bounce < _settings.maxBounces)
            didHit = _scene->intersect(ray, data, info);
    }

    if (!didHit) {
        if (!medium && bounce > _settings.minBounces && _scene->intersectInfinites(ray, data, info))
            result += throughput*info.primitive->evalDirect(data, info);
        return result;
    }
    if (info.primitive->isEmissive() && bounce > _settings.minBounces)
        result += throughput*info.primitive->evalDirect(data, info);

    int count = surfaceTree.nearestNeighbours(ray.hitpoint(), _photonQuery.get(), _distanceQuery.get(),
            _settings.gatherCount, gatherRadius);
    if (count == 0)
        return result;

    const Bsdf &bsdf = *info.bsdf;
    SurfaceScatterEvent event = makeLocalScatterEvent(data, info, ray, &sampler);

    Vec3f surfaceEstimate(0.0f);
    for (int i = 0; i < count; ++i) {
        int fullPathBounce = bounce + _photonQuery[i]->bounce - 1;
        if (fullPathBounce < _settings.minBounces || fullPathBounce >= _settings.maxBounces)
            continue;

        event.wo = event.frame.toLocal(-_photonQuery[i]->dir);
        // Asymmetry due to shading normals already compensated for when storing the photon,
        // so we don't use the adjoint BSDF here
        surfaceEstimate += _photonQuery[i]->power*bsdf.eval(event, false)/std::abs(event.wo.z());
    }
    float radiusSq = count == int(_settings.gatherCount) ? _distanceQuery[0] : gatherRadius*gatherRadius;
    result += throughput*surfaceEstimate*(INV_PI/radiusSq);

    return result;
}

}
