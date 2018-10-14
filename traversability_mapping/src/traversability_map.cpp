#include "utility.h"

class TraversabilityMapping{

private:

    // ROS Node Handle
    ros::NodeHandle nh;
    // Mutex Memory Lock
    std::mutex mtx;
    // Transform Listener
    tf::TransformListener listener;
    tf::StampedTransform transform;
    // Subscriber
    ros::Subscriber subFilteredGroundCloud;
    // Publisher
    ros::Publisher pubOccupancyMapLocal;
    ros::Publisher pubOccupancyMapLocalHeight;
    ros::Publisher pubElevationCloud;
    // Point Cloud Pointer
    pcl::PointCloud<PointType>::Ptr laserCloud; // save input filtered laser cloud for mapping
    pcl::PointCloud<PointType>::Ptr laserCloudElevation; // a cloud for publishing elevation map
    // Occupancy Grid Map
    nav_msgs::OccupancyGrid occupancyMap2D; // local occupancy grid map
    elevation_msgs::OccupancyElevation occupancyMap2DHeight; // customized message that includes occupancy map and elevation info

    int pubCount;
    
    // Map Arrays
    int mapArrayCount;
    int **mapArrayInd; // it saves the index of this submap in vector mapArray
    int **predictionArrayFlag;
    vector<childMap_t*> mapArray;

    // Local Map Extraction
    PointType robotPoint;
    PointType localMapOriginPoint;
    grid_t localMapOriginGrid;

    // Global Variables for Traversability Calculation
    cv::Mat matCov, matEig, matVec;

    // Lists for New Scan
    vector<mapCell_t*> newScanList;
    vector<mapCell_t*> newScanList2; // for passing to prediction list: observingList
    vector<mapCell_t*> observingList; // all grids that are observed in each new scan (updated every scan)

public:
    TraversabilityMapping():
        nh("~"),
        pubCount(10),
        mapArrayCount(0){
        // subscribe to traversability filter
        subFilteredGroundCloud = nh.subscribe<sensor_msgs::PointCloud2>("/filtered_pointcloud", 1, &TraversabilityMapping::cloudHandler, this);
        // publish local occupancy and elevation grid map
        pubOccupancyMapLocal = nh.advertise<nav_msgs::OccupancyGrid> ("/occupancy_map_local", 1);
        pubOccupancyMapLocalHeight = nh.advertise<elevation_msgs::OccupancyElevation> ("/occupancy_map_local_height", 1);
        // publish elevation map for visualization
        pubElevationCloud = nh.advertise<sensor_msgs::PointCloud2> ("/elevation_pointcloud", 1);

        allocateMemory(); 
    }

    ~TraversabilityMapping(){}

    

    void allocateMemory(){
        // allocate memory for point cloud
        laserCloud.reset(new pcl::PointCloud<PointType>());
        laserCloudElevation.reset(new pcl::PointCloud<PointType>());
        
        // initialize array for cmap
        mapArrayInd = new int*[mapArrayLength];
        for (int i = 0; i < mapArrayLength; ++i)
            mapArrayInd[i] = new int[mapArrayLength];

        for (int i = 0; i < mapArrayLength; ++i)
            for (int j = 0; j < mapArrayLength; ++j)
                mapArrayInd[i][j] = -1;

        // initialize array for predicting elevation sub-maps
        predictionArrayFlag = new int*[mapArrayLength];
        for (int i = 0; i < mapArrayLength; ++i)
            predictionArrayFlag[i] = new int[mapArrayLength];

        for (int i = 0; i < mapArrayLength; ++i)
            for (int j = 0; j < mapArrayLength; ++j)
                predictionArrayFlag[i][j] = false;

        // Matrix Initialization
        matCov = cv::Mat (3, 3, CV_32F, cv::Scalar::all(0));
        matEig = cv::Mat (1, 3, CV_32F, cv::Scalar::all(0));
        matVec = cv::Mat (3, 3, CV_32F, cv::Scalar::all(0));

        initializeLocalOccupancyMap();
    }


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////// Mapping /////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    void cloudHandler(const sensor_msgs::PointCloud2ConstPtr& laserCloudMsg){

        if (getRobotPosition() == false) // Get Robot Position
            return;

        // Convert Point Cloud
        pcl::fromROSMsg(*laserCloudMsg, *laserCloud);

        std::lock_guard<std::mutex> lock(mtx);

        // Register New Scan
        updateElevationMap();

        // publish local occupancy grid map
        publishMap();
    }
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////// Register Cloud /////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    void updateElevationMap(){
        int cloudSize = laserCloud->points.size();
        for (int i = 0; i < cloudSize; ++i){
            laserCloud->points[i].z -= 0.2; // for visualization
            updateElevationMap(&laserCloud->points[i]);
        }
    }

    void updateElevationMap(PointType *point){
        // Find point index in global map
        grid_t thisGrid;
        if (findPointGridInMap(&thisGrid, point) == false) return;
        // Get current cell pointer
        mapCell_t *thisCell = grid2Cell(&thisGrid);
        // update elevation
        updateCellElevation(thisCell, point);
        // update occupancy
        updateCellOccupancy(thisCell, point);
        // update observation time
        updateCellObservationTime(thisCell);
    }

    void updateCellObservationTime(mapCell_t *thisCell){
        ++thisCell->observeTimes;
        if (thisCell->observeTimes >= 10){
            // push to a list
            thisCell->observeTimes = 0;
        }
    }

    void updateCellOccupancy(mapCell_t *thisCell, PointType *point){
        // Update log_odds
        float p;  // Probability of being occupied knowing current measurement.
        if (point->intensity == 100)
            p = p_occupied_when_laser;
        else
            p = p_occupied_when_no_laser;
        thisCell->log_odds += std::log(p / (1 - p));

        if (thisCell->log_odds < -large_log_odds)
            thisCell->log_odds = -large_log_odds;
        else if (thisCell->log_odds > large_log_odds)
            thisCell->log_odds = large_log_odds;
        // Update occupancy
        float occupancy;
        if (thisCell->log_odds < -max_log_odds_for_belief)
            occupancy = 0;
        else if (thisCell->log_odds > max_log_odds_for_belief)
            occupancy = 100;
        else
            occupancy = (int)(lround((1 - 1 / (1 + std::exp(thisCell->log_odds))) * 100));
        // update cell
        thisCell->updateOccupancy(occupancy);
    }

    void updateCellElevation(mapCell_t *thisCell, PointType *point){
        // Kalman Filter: update cell elevation using Kalman filter
        // https://www.cs.cornell.edu/courses/cs4758/2012sp/materials/MI63slides.pdf

        // cell is observed for the first time, no need to use Kalman filter
        if (thisCell->elevation == -FLT_MAX){
            thisCell->elevation = point->z;
            thisCell->elevationVar = pointDistance(robotPoint, *point);
            return;
        }

        // Predict:
        float x_pred = thisCell->elevation; // x = F * x + B * u
        float P_pred = thisCell->elevationVar + 0.01; // P = F*P*F + Q
        // Update:
        float R = pointDistance(robotPoint, *point); // measurement noise: R
        float K = P_pred / (P_pred + R);// Gain: K  = P * H^T * (HPH + R)^-1
        float y = point->z; // measurement: y
        float x_final = x_pred + K * (y - x_pred); // x_final = x_pred + K * (y - H * x_pred)
        float P_final = (1 - K) * P_pred; // P_final = (I - K * H) * P_pred
        // Update cell
        thisCell->updateElevation(x_final, P_final);
    }

    mapCell_t* grid2Cell(grid_t *thisGrid){
        return mapArray[mapArrayInd[thisGrid->cubeX][thisGrid->cubeY]]->cellArray[thisGrid->gridX][thisGrid->gridY];
    }

    bool findPointGridInMap(grid_t *gridOut, PointType *point){
        // Calculate the cube index that this point belongs to. (Array dimension: mapArrayLength * mapArrayLength)
        grid_t thisGrid;
        getPointCubeIndex(&thisGrid.cubeX, &thisGrid.cubeY, point);
        // Decide whether a point is out of pre-allocated map
        if (thisGrid.cubeX >= 0 && thisGrid.cubeX < mapArrayLength && 
            thisGrid.cubeY >= 0 && thisGrid.cubeY < mapArrayLength){
            // Point is in the boundary, but this sub-map is not allocated before
            // Allocate new memory for this sub-map and save it to mapArray
            if (mapArrayInd[thisGrid.cubeX][thisGrid.cubeY] == -1){
                childMap_t *thisChildMap = new childMap_t(mapArrayCount, thisGrid.cubeX, thisGrid.cubeY);
                mapArray.push_back(thisChildMap);
                mapArrayInd[thisGrid.cubeX][thisGrid.cubeY] = mapArrayCount;
                ++mapArrayCount;
            }
        }else{
            // Point is out of pre-allocated boundary, report error (you should increase map size)
            ROS_ERROR("Point cloud is out of elevation map boundary. Change params ->mapArrayLength<-. The program will crash!");
            return false;
        }
        // sub-map id
        thisGrid.mapID = mapArrayInd[thisGrid.cubeX][thisGrid.cubeY];
        // Find the index for this point in this sub-map (grid index)
        thisGrid.gridX = (int)((point->x - mapArray[thisGrid.mapID]->originX) / mapResolution);
        thisGrid.gridY = (int)((point->y - mapArray[thisGrid.mapID]->originY) / mapResolution);
        if (thisGrid.gridX < 0 || thisGrid.gridY < 0 || thisGrid.gridX >= mapCubeArrayLength || thisGrid.gridY >= mapCubeArrayLength)
            return false;

        *gridOut = thisGrid;
        return true;
    }

    void getPointCubeIndex(int *cubeX, int *cubeY, PointType *point){
        *cubeX = int((point->x + mapCubeLength/2.0) / mapCubeLength) + rootCubeIndex;
        *cubeY = int((point->y + mapCubeLength/2.0) / mapCubeLength) + rootCubeIndex;

        if (point->x + mapCubeLength/2.0 < 0)  --*cubeX;
        if (point->y + mapCubeLength/2.0 < 0)  --*cubeY;
    }


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////// Traversability Calculation ///////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // void traversabilityMapCalculation(){

    //     // no new scan, return
    //     if (observingList.size() == 0)
    //         return;

    //     int listSize = observingList.size();

    //     for (int i = 0; i < listSize; ++i){

    //         mapCell_t *thisCell = observingList[i];

    //         // if grid non-traversable decided by traversability_filter
    //         if (thisCell->observedNonTraversableFlag == true){
    //             thisCell->updateTraversabilityAlphaBeta(1);
    //             continue;
    //         }
            
    //         vector<float> xyzVector = findNeighborElevations(thisCell);

    //         if (xyzVector.size() <= 2)
    //             continue;

    //         Eigen::MatrixXf matPoints = Eigen::Map<const Eigen::Matrix<float, -1, -1, Eigen::RowMajor>>(xyzVector.data(), xyzVector.size() / 3, 3);

    //         // min and max elevation
    //         float minElevation = matPoints.col(2).minCoeff();
    //         float maxElevation = matPoints.col(2).maxCoeff();
    //         float maxDifference = (maxElevation - minElevation) < 0.05 ? 0 : maxElevation - minElevation;

    //         // if (maxElevation - minElevation > _stepHeightThreshold){
    //         //     thisCell->updateTraversabilityAlphaBeta(1);
    //         //     continue;
    //         // }

    //         // find maximum step
    //         // float maximumStep = -FLT_MAX;
    //         // for (int j = 0; j < matPoints.rows(); ++j){
    //         //     for (int k = j+1; k < matPoints.rows(); ++k){
    //         //         float thisStepDiff = std::abs(matPoints(j,2) - matPoints(k,2));
    //         //         maximumStep = std::max(thisStepDiff, maximumStep);
    //         //     }
    //         // }

    //         Eigen::MatrixXf centered = matPoints.rowwise() - matPoints.colwise().mean();
    //         Eigen::MatrixXf cov = (centered.adjoint() * centered);
    //         cv::eigen2cv(cov, matCov); // copy data from eigen to cv::Mat
    //         cv::eigen(matCov, matEig, matVec); // find eigenvalues and eigenvectors for the covariance matrix

    //         float slopeAngle = std::acos(std::abs(matVec.at<float>(2, 2))) / M_PI * 180;
    //         // float occupancy = 1.0f / (1.0f + exp(-(slopeAngle - _slopeAngleThreshold)));

    //         float occupancy = 0.5 * (slopeAngle / _slopeAngleThreshold)
    //                         + 0.5 * (maxDifference / _stepHeightThreshold);

    //         if (slopeAngle > _slopeAngleThreshold || maxDifference > _stepHeightThreshold)
    //             occupancy = 1;

    //         thisCell->updateTraversabilityAlphaBeta(occupancy);
    //     }
    // }

    // vector<float> findNeighborElevations(mapCell_t *centerCell){

    //     vector<float> xyzVector;

    //     grid_t centerGrid = centerCell->grid;
    //     grid_t thisGrid;

    //     for (int k = -footprintRadiusLength; k <= footprintRadiusLength; ++k){
    //         for (int l = -footprintRadiusLength; l <= footprintRadiusLength; ++l){
    //             // skip grids too far
    //             if (std::sqrt(float(k*k + l*l)) * mapResolution > robotRadius)
    //                 continue;
    //             // the neighbor grid
    //             thisGrid.cubeX = centerGrid.cubeX;
    //             thisGrid.cubeY = centerGrid.cubeY;
    //             thisGrid.gridX = centerGrid.gridX + k;
    //             thisGrid.gridY = centerGrid.gridY + l;
    //             // If the checked grid is in another sub-map, update it's indexes
    //             if(thisGrid.gridX < 0){ --thisGrid.cubeX; thisGrid.gridX = thisGrid.gridX + mapCubeArrayLength;
    //             }else if(thisGrid.gridX >= mapCubeArrayLength){ ++thisGrid.cubeX; thisGrid.gridX = thisGrid.gridX - mapCubeArrayLength; }
    //             if(thisGrid.gridY < 0){ --thisGrid.cubeY; thisGrid.gridY = thisGrid.gridY + mapCubeArrayLength;
    //             }else if(thisGrid.gridY >= mapCubeArrayLength){ ++thisGrid.cubeY; thisGrid.gridY = thisGrid.gridY - mapCubeArrayLength; }
    //             // If the sub-map that the checked grid belongs to is empty or not
    //             int mapInd = mapArrayInd[thisGrid.cubeX][thisGrid.cubeY];
    //             if (mapInd == -1) continue;
    //             // the neighbor cell
    //             mapCell_t *thisCell = grid2Cell(&thisGrid);
    //             // save neighbor cell for calculating traversability
    //             if (thisCell->elevation != -FLT_MAX){
    //                 xyzVector.push_back(thisCell->xyz->x);
    //                 xyzVector.push_back(thisCell->xyz->y);
    //                 xyzVector.push_back(thisCell->xyz->z);
    //             }
    //         }
    //     }

    //     return xyzVector;
    // }


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////// Occupancy Map (local) //////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    void publishMap(){
        // Publish Occupancy Grid Map and Elevation Map
        pubCount++;
        if (pubCount >= 0){
            pubCount = 0;
            publishLocalMap();
            publishTraversabilityMap();
        }
    }

    void publishLocalMap(){

        if (pubOccupancyMapLocal.getNumSubscribers() == 0 &&
            pubOccupancyMapLocalHeight.getNumSubscribers() == 0)
            return;

        // 1.3 Initialize local occupancy grid map to unknown, height to -FLT_MAX
        std::fill(occupancyMap2DHeight.occupancy.data.begin(), occupancyMap2DHeight.occupancy.data.end(), -1);
        std::fill(occupancyMap2DHeight.height.begin(), occupancyMap2DHeight.height.end(), -FLT_MAX);
        std::fill(occupancyMap2DHeight.costMap.begin(), occupancyMap2DHeight.costMap.end(), 0);
        
        // local map origin x and y
        localMapOriginPoint.x = robotPoint.x - localMapLength / 2;
        localMapOriginPoint.y = robotPoint.y - localMapLength / 2;
        localMapOriginPoint.z = robotPoint.z;
        // local map origin cube id (in global map)
        localMapOriginGrid.cubeX = int((localMapOriginPoint.x + mapCubeLength/2.0) / mapCubeLength) + rootCubeIndex;
        localMapOriginGrid.cubeY = int((localMapOriginPoint.y + mapCubeLength/2.0) / mapCubeLength) + rootCubeIndex;
        if (localMapOriginPoint.x + mapCubeLength/2.0 < 0)  --localMapOriginGrid.cubeX;
        if (localMapOriginPoint.y + mapCubeLength/2.0 < 0)  --localMapOriginGrid.cubeY;
        // local map origin grid id (in sub-map)
        float originCubeOriginX, originCubeOriginY; // the orign of submap that the local map origin belongs to (note the submap may not be created yet, cannot use originX and originY)
        originCubeOriginX = (localMapOriginGrid.cubeX - rootCubeIndex) * mapCubeLength - mapCubeLength/2.0;
        originCubeOriginY = (localMapOriginGrid.cubeY - rootCubeIndex) * mapCubeLength - mapCubeLength/2.0;
        localMapOriginGrid.gridX = int((localMapOriginPoint.x - originCubeOriginX) / mapResolution);
        localMapOriginGrid.gridY = int((localMapOriginPoint.y - originCubeOriginY) / mapResolution);

        // 2 Calculate local occupancy grid map root position
        occupancyMap2DHeight.header.stamp = ros::Time::now();
        occupancyMap2DHeight.occupancy.header.stamp = occupancyMap2DHeight.header.stamp;
        occupancyMap2DHeight.occupancy.info.origin.position.x = localMapOriginPoint.x;
        occupancyMap2DHeight.occupancy.info.origin.position.y = localMapOriginPoint.y;
        occupancyMap2DHeight.occupancy.info.origin.position.z = localMapOriginPoint.z + 10; // add 10, just for visualization

        // extract all info
        for (int i = 0; i < localMapArrayLength; ++i){
            for (int j = 0; j < localMapArrayLength; ++j){

                int indX = localMapOriginGrid.gridX + i;
                int indY = localMapOriginGrid.gridY + j;

                grid_t thisGrid;

                thisGrid.cubeX = localMapOriginGrid.cubeX + indX / mapCubeArrayLength;
                thisGrid.cubeY = localMapOriginGrid.cubeY + indY / mapCubeArrayLength;

                thisGrid.gridX = indX % mapCubeArrayLength;
                thisGrid.gridY = indY % mapCubeArrayLength;

                // if sub-map is not created yet
                if (mapArrayInd[thisGrid.cubeX][thisGrid.cubeY] == -1) {
                    continue;
                }
                
                mapCell_t *thisCell = grid2Cell(&thisGrid);

                // skip unknown grid
                if (thisCell->elevation != -FLT_MAX){
                    int index = i + j * localMapArrayLength; // index of the 1-D array 
                    occupancyMap2DHeight.height[index] = thisCell->elevation;
                    occupancyMap2DHeight.occupancy.data[index] = thisCell->occupancy > 50 ? 100 : 0;
                }
            }
        }

        pubOccupancyMapLocalHeight.publish(occupancyMap2DHeight);
        pubOccupancyMapLocal.publish(occupancyMap2DHeight.occupancy);
    }
    

    void initializeLocalOccupancyMap(){
        // initialization of customized map message
        occupancyMap2DHeight.header.frame_id = "map";
        occupancyMap2DHeight.occupancy.info.width = localMapArrayLength;
        occupancyMap2DHeight.occupancy.info.height = localMapArrayLength;
        occupancyMap2DHeight.occupancy.info.resolution = mapResolution;
        
        occupancyMap2DHeight.occupancy.info.origin.orientation.x = 0.0;
        occupancyMap2DHeight.occupancy.info.origin.orientation.y = 0.0;
        occupancyMap2DHeight.occupancy.info.origin.orientation.z = 0.0;
        occupancyMap2DHeight.occupancy.info.origin.orientation.w = 1.0;

        occupancyMap2DHeight.occupancy.data.resize(occupancyMap2DHeight.occupancy.info.width * occupancyMap2DHeight.occupancy.info.height);
        occupancyMap2DHeight.height.resize(occupancyMap2DHeight.occupancy.info.width * occupancyMap2DHeight.occupancy.info.height);
        occupancyMap2DHeight.costMap.resize(occupancyMap2DHeight.occupancy.info.width * occupancyMap2DHeight.occupancy.info.height);
    }    

    bool getRobotPosition(){
        try{listener.lookupTransform("map","base_link", ros::Time(0), transform); } 
        catch (tf::TransformException ex){ ROS_ERROR("Transfrom Failure."); return false; }

        robotPoint.x = transform.getOrigin().x();
        robotPoint.y = transform.getOrigin().y();
        robotPoint.z = transform.getOrigin().z();

        return true;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////// Point Cloud /////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    void publishTraversabilityMap(){

        if (pubElevationCloud.getNumSubscribers() == 0)
            return;

        // 1. Find robot current cube index
        int currentCubeX = int((robotPoint.x + mapCubeLength/2.0) / mapCubeLength) + rootCubeIndex;
        int currentCubeY = int((robotPoint.y + mapCubeLength/2.0) / mapCubeLength) + rootCubeIndex;
        if (robotPoint.x + mapCubeLength/2.0 < 0)  --currentCubeX;
        if (robotPoint.y + mapCubeLength/2.0 < 0)  --currentCubeY;
        // 2. Loop through all the sub-maps that are nearby
        int visualLength = int(visualizationRadius / mapCubeLength);
        for (int i = -visualLength + currentCubeX; i <= visualLength + currentCubeX; ++i){
            for (int j = -visualLength + currentCubeY; j <= visualLength + currentCubeY; ++j){

                if (i < 0 || i >= mapArrayLength ||  j < 0 || j >= mapArrayLength) continue;

                if (mapArrayInd[i][j] == -1) continue;

                *laserCloudElevation += mapArray[mapArrayInd[i][j]]->cloud;
            }
        }
        // 3. Publish elevation point cloud
        sensor_msgs::PointCloud2 laserCloudTemp;
        pcl::toROSMsg(*laserCloudElevation, laserCloudTemp);
        laserCloudTemp.header.frame_id = "/map";
        laserCloudTemp.header.stamp = ros::Time::now();
        pubElevationCloud.publish(laserCloudTemp);
        // 4. free memory
        laserCloudElevation->clear();
    }
};




int main(int argc, char** argv){

    ros::init(argc, argv, "traversability_mapping");
    
    TraversabilityMapping tMapping;

    ROS_INFO("\033[1;32m---->\033[0m Traversability Mapping Started.");

    ros::spin();

    return 0;
}