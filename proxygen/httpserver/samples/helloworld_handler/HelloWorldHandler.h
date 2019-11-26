//
// Created by lukerobbins2112 on 11/26/19.
//

#pragma once

#include <folly/Memory.h>
#include <proxygen/httpserver/RequestHandler.h>

namespace proxygen{
    class ResponseHandler;
}

namespace HelloWorldService {

    class HelloWorldHandler : public proxygen::RequestHandler {

    public:
        HelloWorldHandler();

        void onRequest(std::unique_ptr<proxygen::HTTPMessage> headers) noexcept override ;

        void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override;

        void onEOM() noexcept override;

        void onUpgrade(proxygen::UpgradeProtocol proto) noexcept override;

        void requestComplete() noexcept override;

        void onError(proxygen::ProxygenError error) noexcept override;

    private:
        std::unique_ptr<folly::IOBuf> body_;
    };
}