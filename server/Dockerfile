FROM golang:1.26-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -o /out/intercom-server ./cmd/intercom-server

FROM alpine:3.21
COPY --from=build /out/intercom-server /usr/local/bin/intercom-server
EXPOSE 8090
ENTRYPOINT ["intercom-server"]
