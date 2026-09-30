import http from 'k6/http';
export const options = { vus: 8, duration: '5s' };
export default function () {
  http.get('http://127.0.0.1:8/');   // 死目标 → 连接秒拒 → 逼近 k6 输出上限
}
