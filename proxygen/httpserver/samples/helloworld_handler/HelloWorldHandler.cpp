//
// Created by lukerobbins2112 on 11/26/19.
//

#include "HelloWorldHandler.h"

#include <proxygen/httpserver/RequestHandler.h>
#include <proxygen/httpserver/ResponseBuilder.h>

using namespace proxygen;

namespace HelloWorldService {

    HelloWorldHandler::HelloWorldHandler() {

    }

    void HelloWorldHandler::onRequest(std::unique_ptr <HTTPMessage> headers) noexcept {

    }

    void HelloWorldHandler::onBody(std::unique_ptr<folly::IOBuf> body) noexcept {
        if (body_){
            body_->prependChain(std::move(body));
        } else {
            body_ = std::move(body);
        }
    }

    void HelloWorldHandler::onEOM() noexcept {
        ResponseBuilder(downstream_)
        .status(200, "OK")
        .header("First Header", "Hello")
        .header("Second Header", "World!")
        .body(folly::IOBuf::copyBuffer("This is the body\n"))
        .sendWithEOM();
    }

    void HelloWorldHandler::onUpgrade(proxygen::UpgradeProtocol proto) noexcept {
        // do nothing
    }

    void HelloWorldHandler::requestComplete() noexcept {
        delete this;
    }

    void HelloWorldHandler::onError(proxygen::ProxygenError error) noexcept {
        delete this;
    }


}