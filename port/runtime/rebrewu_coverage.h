#pragma once
// Compile-time opt-in; ordinary Release builds contain no observer calls.
#ifdef RBREW_ENABLE_COVERAGE
#include <cstdint>
#include <exception>
void rbrew_coverage_event(char kind, uint32_t owner, uint32_t pc);
class RbrewCoverageScope {
    uint32_t owner_, pc_=0; int exceptions_;
public:
    explicit RbrewCoverageScope(uint32_t owner):owner_(owner),exceptions_(std::uncaught_exceptions()) {
        rbrew_coverage_event('E',owner_,0);
    }
    void pc(uint32_t value) {
        if(pc_)rbrew_coverage_event('C',owner_,pc_);
        pc_=value;rbrew_coverage_event('P',owner_,pc_);
    }
    ~RbrewCoverageScope() {
        if(std::uncaught_exceptions()!=exceptions_)return;
        if(pc_)rbrew_coverage_event('C',owner_,pc_);
        rbrew_coverage_event('R',owner_,0);
    }
};
#define RBREW_COVERAGE_FUNCTION(entry) RbrewCoverageScope rbrew_coverage_scope(entry)
#define RBREW_COVERAGE_PC(value) rbrew_coverage_scope.pc(value)
#define RBREW_COVERAGE_EVENT(kind,owner,pc) rbrew_coverage_event(kind,owner,pc)
#else
#define RBREW_COVERAGE_FUNCTION(entry) ((void)0)
#define RBREW_COVERAGE_PC(pc) ((void)0)
#define RBREW_COVERAGE_EVENT(kind,owner,pc) ((void)0)
#endif
