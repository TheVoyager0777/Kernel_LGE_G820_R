#!/bin/sh
case $1 in
    "q")
        echo "Merge qcacld"
        git fetch qcacld-3.0 $2
        git merge -X subtree=drivers/staging/qcacld-3.0 FETCH_HEAD
        ;;
    "w")
        echo "Merge qca-wifi-host-cmn"
        git fetch qca-wifi-host-cmn $2
        git merge -X subtree=drivers/staging/qca-wifi-host-cmn FETCH_HEAD
        ;;
    "f")
        echo "Merge fw-api"
        git fetch fw-api $2
        git merge -X subtree=drivers/staging/fw-api FETCH_HEAD
        ;;
    "a")
        echo "Merge qcacld"
        git fetch qcacld-3.0 $2
        git merge -X subtree=drivers/staging/qcacld-3.0 FETCH_HEAD
        echo "Merge qca-wifi-host-cmn"
        git fetch qca-wifi-host-cmn $2
        git merge -X subtree=drivers/staging/qca-wifi-host-cmn FETCH_HEAD
        echo "Merge fw-api"
        git fetch fw-api $2
        git merge -X subtree=drivers/staging/fw-api FETCH_HEAD
        ;;
    *)
esac
