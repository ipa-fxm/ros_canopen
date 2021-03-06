#include <canopen_master/canopen.h>

using namespace canopen;

#pragma pack(push) /* push current alignment to stack */
#pragma pack(1) /* set alignment to 1 byte boundary */


struct NMTcommand{
    enum Command{
        Start = 1,
        Stop = 2,
        Prepare = 128,
        Reset = 129,
        Reset_Com = 130
    };
    uint8_t command;
    uint8_t node_id;
    
    struct Frame: public FrameOverlay<NMTcommand>{
        Frame(uint8_t node_id, const Command &c) : FrameOverlay(can::Header()) {
            data.command = c;
            data.node_id = node_id;
        }
    };
};

#pragma pack(pop) /* pop previous alignment from stack */

Node::Node(const boost::shared_ptr<can::CommInterface> interface, const boost::shared_ptr<ObjectDict> dict, uint8_t node_id, const boost::shared_ptr<SyncCounter> sync)
: Layer("Node 301"), node_id_(node_id), interface_(interface), sync_(sync) , state_(Unknown), sdo_(interface, dict, node_id), emcy_(interface, getStorage()), pdo_(interface){
    try{
        getStorage()->entry(heartbeat_, 0x1017);
    }
    catch(const std::out_of_range){
    }
}
    
const Node::State Node::getState(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    return state_;
}

bool Node::reset_com(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    getStorage()->reset();
    interface_->send(NMTcommand::Frame(node_id_, NMTcommand::Reset_Com));
    if(wait_for(BootUp, boost::chrono::seconds(10)) != 1){
        return false;
    }
    state_ = PreOperational;
    setHeartbeatInterval();
    return true;
}
bool Node::reset(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    getStorage()->reset();
    
    interface_->send(NMTcommand::Frame(node_id_, NMTcommand::Reset));
    if(wait_for(BootUp, boost::chrono::seconds(10)) != 1){
        return false;
    }
    state_ = PreOperational;
    setHeartbeatInterval();
    return true;
}

bool Node::prepare(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    if(state_ == BootUp){
        // ERROR
    }
    interface_->send(NMTcommand::Frame(node_id_, NMTcommand::Prepare));
    return 0 != wait_for(PreOperational, boost::chrono::seconds(2));
}
bool Node::start(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    if(state_ == BootUp){
        // ERROR
    }
    interface_->send(NMTcommand::Frame(node_id_, NMTcommand::Start));
    return 0 != wait_for(Operational, boost::chrono::seconds(2));
}
bool Node::stop(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    if(sync_) sync_->removeNode(this);
    if(state_ == BootUp){
        // ERROR
    }
    interface_->send(NMTcommand::Frame(node_id_, NMTcommand::Stop));
    return 0 != wait_for(Stopped, boost::chrono::seconds(2));
}

void Node::switchState(const uint8_t &s){
    switch(s){
        case Operational:
            if(sync_) sync_->addNode(this);
            break;
        case BootUp:
        case PreOperational:
        case Stopped:
            if(sync_) sync_->removeNode(this);
            break;
        default:
            //error
            ;
    }
    state_ = (State) s;
    state_dispatcher_.dispatch(state_);
}
void Node::handleNMT(const can::Frame & msg){
    boost::mutex::scoped_lock cond_lock(cond_mutex);
    heartbeat_timeout_ = boost::chrono::high_resolution_clock::now() + boost::chrono::milliseconds(3*heartbeat_.get_cached());
    assert(msg.dlc == 1);
    switchState(msg.data[0]);
    cond_lock.unlock();
    cond.notify_one();
    
}
template<typename T> int Node::wait_for(const State &s, const T &timeout){
    boost::mutex::scoped_lock cond_lock(cond_mutex);
    time_point abs_time = get_abs_time(timeout);

    while(s != state_) {
        if(cond.wait_until(cond_lock,abs_time) == boost::cv_status::timeout)
        {
            break;
        }
    }
    if( s!= state_){
        if(getHeartbeatInterval() == 0){
            switchState(s);
            return -1;
        }
        return 0;
    }
    return 1;
}
bool Node::checkHeartbeat(){
    if(!heartbeat_.get_cached()) return true; //disabled
    boost::mutex::scoped_lock cond_lock(cond_mutex);
    return heartbeat_timeout_ >= boost::chrono::high_resolution_clock::now();
}


void Node::handleRead(LayerStatus &status, const LayerState &current_state) {
    if(current_state > Init){
        if(!checkHeartbeat()){
            status.error("heartbeat problem");
        } else if(getState() != Operational){
            status.error("not operational");
        } else{
            pdo_.read(status);
            emcy_.read(status);
        }
    }
}
void Node::handleWrite(LayerStatus &status, const LayerState &current_state) {
    if(current_state > Init){
        if(getState() != Operational)  status.error("not operational");
        else if(! pdo_.write())  status.error("PDO write problem");
    }
}


void Node::handleDiag(LayerReport &report){
    State state = getState();
    if(state != Operational){
        report.error("Mode not operational");
        report.add("Node state", (int)state);
    }else if(!checkHeartbeat()){
        report.error("Heartbeat timeout");
    }
    if(state != Unknown) emcy_.diag(report);
}
void Node::handleInit(LayerStatus &status){
    nmt_listener_ = interface_->createMsgListener( can::MsgHeader(0x700 + node_id_), can::CommInterface::FrameDelegate(this, &Node::handleNMT));

    sdo_.init();
    try{
        if(!reset_com()) BOOST_THROW_EXCEPTION( TimeoutException("reset_timeout") );
    }
    catch(const TimeoutException&){
        status.error(boost::str(boost::format("could not reset node '%1%'") % (int)node_id_));
        return;
    }

    if(!pdo_.init(getStorage(), status)){
        return;
    }
    getStorage()->init_all();
    sdo_.init(); // reread SDO paramters;
    // TODO: set SYNC data

    try{
        if(!start()) BOOST_THROW_EXCEPTION( TimeoutException("start timeout") );
    }
    catch(const TimeoutException&){
        status.error(boost::str(boost::format("could not start node '%1%'") %  (int)node_id_));
    }
    emcy_.init();
}
void Node::handleRecover(LayerStatus &status){
    emcy_.recover();
    if(getState() != Operational){
        try{
            start();
        }
        catch(const TimeoutException&){
            status.error(boost::str(boost::format("could not start node '%1%'") %  (int)node_id_));
        }
    }
}
void Node::handleShutdown(LayerStatus &status){
    stop();
    if(getHeartbeatInterval()> 0) heartbeat_.set(0);
    nmt_listener_.reset();
    switchState(Unknown);
}
void Node::handleHalt(LayerStatus &status){
    // do nothing
}
