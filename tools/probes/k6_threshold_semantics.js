import http from 'k6/http';
export const options = {
  vus: 2, duration: '2s',
  thresholds: {
    'http_req_duration': ['p(95)<100000', 'p(95)<0.0001'],   // 第一个必然满足，第二个必然被突破
  },
};
export default function () { http.get('http://127.0.0.1:8899/'); }
