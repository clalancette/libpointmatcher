//
// Created by Matěj Boxan on 2023-10-09.
//

#include "Symmetry.h"
#include "MatchersImpl.h"


// SymmetryDataPointsFilter
// Constructor
template<typename T>
SymmetryDataPointsFilter<T>::SymmetryDataPointsFilter(const Parameters& params):
        PointMatcher<T>::DataPointsFilter("SymmetryDataPointsFilter",
                                          SymmetryDataPointsFilter::availableParameters(), params),
        vrs(Parametrizable::get<T>("vrs")),
        vro(Parametrizable::get<T>("vro")),
        dt(Parametrizable::get<T>("dt")),
        ct(Parametrizable::get<T>("ct")),
        initialVariance(Parametrizable::get<T>("initialVariance")),
        knn(Parametrizable::get<unsigned>("knn"))
{
}

// Compute
template<typename T>
typename PointMatcher<T>::DataPoints SymmetryDataPointsFilter<T>::filter(
        const DataPoints& input)
{
    DataPoints output(input);
    inPlaceFilter(output);
    return output;
}

// In-place filter
template<typename T>
void SymmetryDataPointsFilter<T>::inPlaceFilter(
        DataPoints& cloud)
{
    // TODO allow 2D case
    unsigned dim = 3;
    assert(cloud.getEuclideanDim() == dim);
    if(!cloud.descriptorExists("omega"))
    {
        Matrix omegas = Matrix::Zero(1, cloud.getNbPoints());
        omegas.setOnes();
        cloud.addDescriptor("omega", omegas);
    }
    if(!cloud.descriptorExists("deviation"))
    {
        Matrix deviations = Matrix::Zero(std::pow(dim, 2), cloud.getNbPoints());
        if(dim == 2)
        {
            deviations.row(0) = PM::Matrix::Constant(1, cloud.getNbPoints(), initialVariance);
            deviations.row(3) = PM::Matrix::Constant(1, cloud.getNbPoints(), initialVariance);
        }
        else
        {
            deviations.row(0) = PM::Matrix::Constant(1, cloud.getNbPoints(), initialVariance);
            deviations.row(4) = PM::Matrix::Constant(1, cloud.getNbPoints(), initialVariance);
            deviations.row(8) = PM::Matrix::Constant(1, cloud.getNbPoints(), initialVariance);
        }

        cloud.addDescriptor("deviation", deviations);
    }
    assert(cloud.getDescriptorDimension("omega") == 1);
    // TODO only store upper diagonal
    assert(cloud.getDescriptorDimension("deviation") == std::pow(dim, 2));
    int updated_ctr = 2;

    auto distributions = getDistributionsFromCloud(cloud);

    while(updated_ctr > 0)
    {
        updated_ctr -= 1;
        auto number_of_points_before_sampling = static_cast<float>(distributions.size());
        if(updated_ctr % 2 == 1) // symmetry sampling
        {
            symmetrySampling(distributions);
        }
        else // overlap sampling
        {
            overlapSampling(distributions);
        }
        auto number_of_points_after_sampling = static_cast<float>(distributions.size());
        std::cout << "Down to " << number_of_points_after_sampling << " points\n";
        if(number_of_points_after_sampling / number_of_points_before_sampling < ct)
        {
            if(updated_ctr == 0)
            {
                updated_ctr = 2;
            }
        }
        else
        {
            std::cout << "Almost no points removed\n";
        }
    }
    cloud = getCloudFromDistributions(cloud, distributions);
}

template<typename T>
void SymmetryDataPointsFilter<T>::symmetrySampling(
        std::vector<std::shared_ptr<Distribution<T>>>& distributions)
{
    std::cout << "Symmetry sampling" << std::endl;

    using namespace PointMatcherSupport;

	typedef typename MatchersImpl<T>::KDTreeMatcher KDTreeMatcher;
	typedef typename PointMatcher<T>::Matches Matches;

    const int pointsCount(distributions.size());

    Parametrizable::Parameters param;
    boost::assign::insert(param)("knn", toParam(std::min(knn, (unsigned) distributions.size())));

    auto cloud = getPointsFromDistributions(distributions);

    // Build kd-tree
    KDTreeMatcher matcher(param);
    matcher.init(cloud);

    Matches matches(typename Matches::Dists(knn, pointsCount), typename Matches::Ids(knn, pointsCount));
    matches = matcher.findClosests(cloud);

    Vector masks_all = Vector::Ones(pointsCount);

    for(int i = 0; i < pointsCount; ++i)
    {
        if(masks_all(i) != 1)
        {
            continue;
        }
        auto distro1 = distributions[i];
        std::vector<unsigned> mergedIndexes;
        for(int j = 1; j < int(knn); ++j) // index from 1 to skip self-match
        {
            if(matches.dists(j, i) == Matches::InvalidDist || matches.ids(j, i) == Matches::InvalidId)
            {
                continue;
            }

            unsigned m = matches.ids(j, i);

            if(masks_all(m) != 1)
            {
                continue;
            }
            auto distro2 = distributions[m];
            float volume2 = distro2->getVolume();

            bool was_merge = false;

            boost::optional<Distribution<T>> combined_distro;
            for(unsigned k = j + 1; k < knn; ++k)
            {
                if(matches.dists(k, i) == Matches::InvalidDist || matches.ids(k, i) == Matches::InvalidId)
                {
                    continue;
                }
                unsigned neighbor_idx = matches.ids(k, i);
                if(masks_all(neighbor_idx) != 1)
                {
                    continue;
                }
                auto distro3 = distributions[neighbor_idx];
                auto point3 = cloud.features.col(neighbor_idx).head(3);
                auto delta = distro2->point - distro3->point;
                auto closest_point = point3 + (1. / (distro2->omega + distro3->omega) * distro2->omega * delta);
                float distance = (closest_point - distro1->point).norm();

                if(distance < dt)
                {
                    float volume3 = distro3->getVolume();
                    Distribution<T> distro_c = Distribution<T>::combineDistros(*distro2, *distro3);

                    float volume_c = distro_c.getVolume();
                    float sum_of_volumes = volume2 + volume3;
                    float ratio = volume_c / sum_of_volumes;

                    if(ratio < vrs)
                    {
                        masks_all(m) = 0;
                        masks_all(neighbor_idx) = 0;
                        was_merge = true;
                        combined_distro = distro_c;
                        mergedIndexes.push_back(m);
                        mergedIndexes.push_back(neighbor_idx);
                        break;
                    }
                }
            }
            if(combined_distro)
            {
                masks_all(i) = 2;
                mergedIndexes.push_back(i);
                Distribution<T> new_distro = Distribution<T>::combineDistros(*combined_distro, *distro1, 3);
                distributions[i] = std::make_shared<Distribution<T>>(new_distro);
            }

            if(was_merge)
            {
                break;
            }
        }
    }

    std::vector<std::shared_ptr<Distribution<T>>> distributions_out;
    std::vector<std::shared_ptr<Distribution<T>>> distributions_out_unused;// TODO this is only needed for testing, to preserve the same order of elements as in the Rust code
    for(unsigned i = 0; i < distributions.size(); ++i)
    {
        if (masks_all(i) == 1) {
            distributions_out_unused.push_back(distributions[i]);
        }
        if (masks_all(i) == 2) {
            distributions_out.push_back(distributions[i]);
        }
    }
    distributions_out.insert(distributions_out.end(),
                                      std::make_move_iterator(distributions_out_unused.begin()),
                                      std::make_move_iterator(distributions_out_unused.end()));
    distributions = distributions_out;
}

template<typename T>
void SymmetryDataPointsFilter<T>::overlapSampling(
        std::vector<std::shared_ptr<Distribution<T>>>& distributions)
{
    std::cout << "Overlap sampling" << std::endl;

    using namespace PointMatcherSupport;

	typedef typename MatchersImpl<T>::KDTreeMatcher KDTreeMatcher;
	typedef typename PointMatcher<T>::Matches Matches;

    const int pointsCount(distributions.size());

    Parametrizable::Parameters param;
    boost::assign::insert(param)("knn", toParam(std::min(knn, (unsigned) distributions.size())));
    auto cloud = getPointsFromDistributions(distributions);

    // Build kd-tree
    KDTreeMatcher matcher(param);
    matcher.init(cloud);

    Matches matches(typename Matches::Dists(knn, pointsCount), typename Matches::Ids(knn, pointsCount));
    matches = matcher.findClosests(cloud);

    Eigen::VectorXd masks_all = Eigen::VectorXd::Ones(pointsCount);

    for(int i = 0; i < pointsCount; ++i)
    {
        if(masks_all(i) != 1)
        {
            continue;
        }

        auto distro1 = distributions[i];

        bool was_overlap = false;
        std::vector<unsigned> mergedIndexes;
        for(int j = 1; j < int(knn); ++j) // index from 1 to skip self-match
        {
            if(matches.dists(j, i) == Matches::InvalidDist || matches.ids(j, i) == Matches::InvalidId)
            {
                continue;
            }

            unsigned m = matches.ids(j, i);

            if(masks_all(m) != 1)
            {
                continue;
            }
            auto distro2 = distributions[m];

            Distribution<T> distro_c = Distribution<T>::combineDistros(*distro2, *distro1, 2);

            float volume_c = distro_c.getVolume();
            float sum_of_volumes = distro1->getVolume() + distro2->getVolume();
            float ratio = volume_c / sum_of_volumes;

            if(ratio < vro)
            {
                masks_all(m) = 0;
                mergedIndexes.push_back(m);
                was_overlap = true;
                distro1 = std::make_shared<Distribution<T>>(distro_c);
            }
        }

        if(was_overlap)
        {
            masks_all(i) = 2;
            mergedIndexes.push_back(i);
            distributions[i] = distro1;
        }
    }

    std::vector<std::shared_ptr<Distribution<T>>> distributions_out;
    std::vector<std::shared_ptr<Distribution<T>>> distributions_out_unused; // TODO this is only needed for testing, to preserve the same order of elements as in the Rust code
    for(unsigned i = 0; i < distributions.size(); ++i)
    {
        if (masks_all(i) == 1) {
            distributions_out_unused.push_back(distributions[i]);
        }
        if (masks_all(i) == 2) {
            distributions_out.push_back(distributions[i]);
        }
    }
    distributions_out.insert(distributions_out.end(),
                                      std::make_move_iterator(distributions_out_unused.begin()),
                                      std::make_move_iterator(distributions_out_unused.end()));
    distributions = distributions_out;
}

template<typename T>
typename PointMatcher<T>::DataPoints SymmetryDataPointsFilter<T>::getCloudFromDistributions(
        const DataPoints& in_cloud, std::vector<std::shared_ptr<Distribution<T>>>& distributions)
{
    DataPoints out_cloud = in_cloud.createSimilarEmpty();
    out_cloud.conservativeResize(distributions.size());

    BOOST_AUTO(omegas, out_cloud.getDescriptorViewByName("omega"));
    BOOST_AUTO(deviations, out_cloud.getDescriptorViewByName("deviation"));

    unsigned ctr = 0;
    out_cloud.features.row(3).setOnes();
    for(const auto &distro: distributions)
    {
        out_cloud.features.col(ctr).head(3) = (*distro).point;
        out_cloud.descriptors.col(ctr) = (*distro).descriptors;
        out_cloud.times.col(ctr) = (*distro).times;
        omegas(0, ctr) = (*distro).omega;
        // Eigen reshaped requires Eigen3.4
//        deviations.col(ctr) = (*distro).deviation.reshaped(9, 1);
        Eigen::Map<Eigen::Matrix<T, 9, 1>> deviationVector((*distro).deviation.data(), 9, 1);
        deviations.col(ctr) = deviationVector;
        ctr += 1;
    }
    return out_cloud;
}

template<typename T>
typename PointMatcher<T>::DataPoints SymmetryDataPointsFilter<T>::getPointsFromDistributions(
        std::vector<std::shared_ptr<Distribution<T>>>& distributions)
{
    DataPoints out;
    out.allocateFeature("x", 1);
    out.allocateFeature("y", 1);
    out.allocateFeature("z", 1);
    out.allocateFeature("pad", 1);

    out.conservativeResize(distributions.size());
    unsigned idx = 0;
    out.features.row(3).setOnes();
    for(const auto &distro: distributions)
    {
        out.features.col(idx).head(3) = (*distro).point;
        idx += 1;
    }
    return out;
}

template<typename T>
std::vector<std::shared_ptr<Distribution<T>>> SymmetryDataPointsFilter<T>::getDistributionsFromCloud(SymmetryDataPointsFilter::DataPoints& cloud)
{
    std::vector<std::shared_ptr<Distribution<T>>> distributions;
    distributions.reserve(cloud.getNbPoints());
    auto points = cloud.features;
    auto omegas = cloud.getDescriptorViewByName("omega");
    auto deviations = cloud.getDescriptorViewByName("deviation");
    for(unsigned i = 0; i < cloud.getNbPoints(); ++i)
    {
        // Eigen reshaped requires Eigen3.4
        Eigen::Map<Eigen::Matrix<T, 3, 3>> deviationMatrix(deviations.block(0, i, 9, 1).data(), 3, 3);
        distributions.emplace_back(new Distribution<T>(points.col(i).head(3),
                                                       omegas(0, i),
                                                       deviationMatrix,
                                                       cloud.times.col(i),
                                                       cloud.descriptors.col(i)));
    }
    return distributions;
}

template<typename T>
void SymmetryDataPointsFilter<T>::mergeTimesDescriptors(std::vector<std::shared_ptr<Distribution<T>>>& distributions, const std::vector<unsigned>& indexesToMerge)
{
    unsigned numOfPoints = indexesToMerge.size();
    const unsigned int mergeTo = indexesToMerge[numOfPoints - 1];

    for(unsigned i = 0; i < numOfPoints-1; ++i)
    {
        distributions[mergeTo]->descriptors += distributions[i]->descriptors;
        distributions[mergeTo]->times += distributions[i]->times;
    }
    distributions[mergeTo]->descriptors /= numOfPoints;
    distributions[mergeTo]->times /= numOfPoints;
}

template
struct SymmetryDataPointsFilter<float>;
template
struct SymmetryDataPointsFilter<double>;